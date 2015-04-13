//============================================================================
// Name        : fileSystem.cpp
// Author      : ziliang wang
// Version     :
// Copyright   : Your copyright notice
// Description : file system
//============================================================================
#define FUSE_USE_VERSION 27
#include <iostream>
#include <map>
#include <utility>
#include <set>
#include <fstream>
#include <vector>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <sstream>
#include <algorithm>
#include <streambuf>
#include <stdexcept>
#include <fusekit/daemon.h>
using namespace std;

string currWorkingDir = "/";

const int ROOT_DIRECTORY_INODE = 26;
const string FILE_NAME_PREFIX = "fusedata.";
const string CURR_WORKING_DIRECTORY = "current_working_directory";
const int DEFAULT_UID = 1000;
const int DEFAULT_GID = 1000;

vector<string> nonInodeInfoKeys;

map<string, int> SUPER_BLOCK;
map<string, int> pathToInodeMap;
map<int, map<string, int> > fileInfoMap;
map<int, string> openedFileMap;
map<string, string> fileSysCallContext;
map<string, int> root;

/***************************helper functions***************************/

//read (count) bytes from a file start at (position)
static string readFile(string fileName, int position, int count) {
	fstream currFile(fileName.c_str());
	char data[count];
	currFile.seekg(position, ios::beg);
	currFile.read(data, count);
	currFile.close();
	return data;
}

//read all the content in the file
static string readAllFile(string fileName) {
	fstream currFile(fileName.c_str());
	string tmpData;
	currFile.seekg(0, ios::end);
	tmpData.reserve(currFile.tellg());
	currFile.seekg(0, ios::beg);
	tmpData.assign((istreambuf_iterator<char>(currFile)),
			istreambuf_iterator<char>());
	currFile.close();
	return tmpData;
}

//write data to a file start at given position
static void writeFile(string fileName, int position, string data) {
	string originalData = readAllFile(fileName);
	fstream currFile(fileName.c_str(), ios::in | ios::out | ios::trunc);
	string newData = originalData.substr(0, position) + data
			+ originalData.substr(position + data.size(), originalData.size());
	if (newData.size() > 4096) {
		newData = newData.substr(0, 4096);
	} else {
		int blankCount = 4096 - newData.size();
		for (int i = 0; i < blankCount; i++) {
			newData += "0";
		}
	}
	currFile << newData;
	currFile.close();
}

//transfer string to integer
static int stringToInt(string num) {
	char *contentEnd;
	int number = strtol(num.c_str(), &contentEnd, 10);
	delete contentEnd;
	return number;
}

//transfer integer to string
static string intToString(int num) {
	char tmp[10];
	sprintf(tmp, "%d", num);
	return tmp;
}

//delete the key from a map
static void deleteFromMap(map<string, int> &currMap, string key) {
	if (currMap.find(key) != currMap.end()) {
		map<string, int>::iterator it = currMap.find(key);
		currMap.erase(it);
	}
}

//delete the key from a map, not used
//static void deleteFromMap(map<int, map<string, int> > &currMap, int key) {
//	if (currMap.find(key) != currMap.end()) {
//		map<int, map<string, int> >::iterator it = currMap.find(key);
//		currMap.erase(it);
//	}
//}

//delete the key from a map
static void deleteFromMap(map<int, string> &currMap, int key) {
	if (currMap.find(key) != currMap.end()) {
		map<int, string>::iterator it = currMap.find(key);
		currMap.erase(it);
	}
}

//insert/update key, value pari in <string,int> map
static void insertIntoMap(map<int, string> &currMap, int key, string value) {
	if (currMap.find(key) == currMap.end()) {
		currMap[key] = value;
	} else {
		map<int, string>::iterator it = currMap.find(key);
		it->second = value;
	}
}

//insert/update key, value pari in <string,int> map
static void insertIntoMap(map<string, int> &currMap, string key, int value) {
	if (currMap.find(key) == currMap.end()) {
		currMap[key] = value;
	} else {
		map<string, int>::iterator it = currMap.find(key);
		it->second = value;
	}
}

//insert map into a map
static void insertIntoMap(map<int, map<string, int> > &currMap, int key,
		map<string, int> value) {
	if (currMap.find(key) != currMap.end()) {
		for (map<string, int>::iterator it = value.begin(); it != value.end();
				++it) {
			insertIntoMap(currMap[key], it->first, value[it->first]);
		}
	} else {
		map<string, int> tmpHolder;
		currMap[key] = tmpHolder;
		for (map<string, int>::iterator it = value.begin(); it != value.end();
				++it) {
			insertIntoMap(currMap[key], it->first, value[it->first]);
		}
	}
}

//put number from start to end to a vector
static void generateNumList(int start, int end, vector<int> &tmpList) {
	for (int i = start; i < end; i++) {
		tmpList.push_back(i);
	}
}

//get absolute path from a path
static string getAbsolutePath(string path) {
	if (path == "") {
		return path;
	}
	if (path.substr(0, 1) != "/") {
		path = fileSysCallContext[CURR_WORKING_DIRECTORY] + "/" + path;
	}
	stringstream ss(path);
	string item;
	vector<string> pathList;
	while (getline(ss, item, '/')) {
		if (item == "" || item == ".") {
			continue;
		} else {
			pathList.push_back(item);
		}
	}
	int position = 0;
	while (position < (int) pathList.size()) {
		if (pathList[position] == "..") {
			if (position > 0) {
				pathList.erase(pathList.begin() + position);
				pathList.erase(pathList.begin() + position - 1);
				position -= 1;
				continue;
			} else {
				pathList.erase(pathList.begin() + position);
				continue;
			}
		} else {
			position++;
		}

	}
	stringstream tmpRes;
	for (size_t i = 0; i < pathList.size(); i++) {
		tmpRes << "/";
		tmpRes << pathList[i];
	}
	if (tmpRes.str().size() == 0) {
		tmpRes << "/";
	}
	return tmpRes.str();
}

//get absolute parent path from a path
static string getAbsParentPath(string path) {
	return getAbsolutePath(path + "/..");
}

//string split
static vector<string> splitString(string original, char delimeter) {
	stringstream ss(original);
	string item;
	vector<string> splitted;
	while (getline(ss, item, delimeter)) {
		if (item == "") {
			continue;
		} else {
			splitted.push_back(item);
		}
	}
	return splitted;
}

//generate file name from given number->fuseData.num
static string getFileName(int num) {
	return FILE_NAME_PREFIX + intToString(num);
}

//get information from a block file which stores a map
static map<string, int> getBlockInfoData(int blockNumber) {
	stringstream ss(readAllFile(getFileName(blockNumber)));
	string tmpStr;
	map<string, int> blockInfo;
	while (getline(ss, tmpStr, '\n')) {
		vector<string> tmpData = splitString(tmpStr, ':');
		blockInfo[tmpData[0]] = stringToInt(tmpData[1]);

	}
	return blockInfo;
}

//get information from a block file which stores numbers
static vector<int> getBlockNumData(int blockNumber) {
	stringstream ss(readAllFile(getFileName(blockNumber)));
	string tmpStr;
	vector<int> blockList;
	while (getline(ss, tmpStr)) {
		if (tmpStr.size() > 0) {
			blockList.push_back(stringToInt(tmpStr));
		}
	}
	return blockList;
}

//serialize a vector to a string
static string serializeVector(vector<int> blockListData) {
	string result = "";
	for (vector<int>::size_type i = 0; i < blockListData.size(); i++) {
		result = result + intToString(blockListData[i]) + "\n";
	}
	return result;
}

//serialize a map to a string
static string serializeStringMap(map<string, int> stringMap) {
	string result = "";
	for (map<string, int>::iterator it = stringMap.begin();
			it != stringMap.end(); ++it) {
		string curr = it->first + ":" + intToString(stringMap[it->first])
				+ "\n";
		result += curr;
	}
	return result;
}

//write block list to file
static void writeNumToBlock(int fileNumber, vector<int> blockListData) {
	ofstream currFile(getFileName(fileNumber).c_str(), ios::trunc);
	string serializedInfo = serializeVector(blockListData);
	currFile << serializedInfo;
	currFile.close();
}

//write a map to a block file
static void writeMaptoBlock(int fileNumber, map<string, int> infoMap) {
	ofstream currFile(getFileName(fileNumber).c_str(), ios::trunc);
	string serializedInfo = serializeStringMap(infoMap);
	currFile << serializedInfo;
	currFile.close();
}

//create a file with name fuseData.fileNumber and 4096 "0" inside
void createFile(int fileNumber) {
	ofstream currFile(getFileName(fileNumber).c_str(), ios::trunc);
	for (int i = 0; i < 4096; i++) {
		currFile << "0";
	}
	currFile.close();
}

//remove all file
void removeAllFile() {
	for (int i = 0; i < 10000; i++) {
		remove(getFileName(i).c_str());
	}
}

//find a free block
int findFreeBlock() {
	for (int i = 1; i < 26; i++) {
		vector<int> blockList = getBlockNumData(i);
		sort(blockList.begin(), blockList.end());
		if (blockList.size() > 0) {
			int freeBlock = blockList[0];
			blockList.erase(blockList.begin());
			writeNumToBlock(i, blockList);
			return freeBlock;
		}
	}
	throw logic_error("No free block available");
	return -1;
}

//find multiple free blocks
vector<int> findMultiFreeBlocks(int num) {
	vector<int> result;
	if (num == 0) {
		return result;
	}
	for (int i = 1; i < 26; i++) {
		vector<int> tmpBlockList = getBlockNumData(i);
		sort(tmpBlockList.begin(), tmpBlockList.end());
		while (tmpBlockList.size() > 0) {
			result.push_back(tmpBlockList[0]);
			tmpBlockList.erase(tmpBlockList.begin());
			if ((int) result.size() == num) {
				writeNumToBlock(i, tmpBlockList);
				return result;
			}
			writeNumToBlock(i, tmpBlockList);
		}
	}
	return result;
}

//add one block to the free block list
void addOneToFreeBlock(int blockNumber) {
	vector<int> data = getBlockNumData(blockNumber / 400 + 1);
	if (find(data.begin(), data.end(), blockNumber) != data.end()) {
		data.push_back(blockNumber);
		sort(data.begin(), data.end());
		writeNumToBlock(blockNumber % 400 + 1, data);
	}
}

//add more than one blocks to the free block list
void addToFreeBlock(vector<int> blockList) {
	for (vector<int>::size_type i = 0; i < blockList.size(); i++) {
		addOneToFreeBlock(blockList[i]);
	}
}

/***************************file system functions***************************/
//init
void init(struct fuse_conn_info *conn) {
	//remove fuseData.x that already exist in the system
	removeAllFile();
	insertIntoMap(pathToInodeMap, "/", ROOT_DIRECTORY_INODE);
	insertIntoMap(SUPER_BLOCK, "creationTime", 1376483073);
	insertIntoMap(SUPER_BLOCK, "mounted", 1);
	insertIntoMap(SUPER_BLOCK, "dev_id", 20);
	insertIntoMap(SUPER_BLOCK, "freeStat", 1);
	insertIntoMap(SUPER_BLOCK, "freeEnd", 25);
	insertIntoMap(SUPER_BLOCK, "root", 26);
	insertIntoMap(SUPER_BLOCK, "maxBlocks", 10000);
	nonInodeInfoKeys.push_back("size");
	nonInodeInfoKeys.push_back("uid");
	nonInodeInfoKeys.push_back("gid");
	nonInodeInfoKeys.push_back("mode");
	nonInodeInfoKeys.push_back("atime");
	nonInodeInfoKeys.push_back("ctime");
	nonInodeInfoKeys.push_back("mtime");
	nonInodeInfoKeys.push_back("linkcount");
	nonInodeInfoKeys.push_back("filename_to_inode_dict");
	insertIntoMap(root, "size", 0);
	insertIntoMap(root, "uid", 1);
	insertIntoMap(root, "gid", 1);
	insertIntoMap(root, "mode", 16877);
	insertIntoMap(root, "atime", 1323630836);
	insertIntoMap(root, "ctime", 1323630836);
	insertIntoMap(root, "mtime", 1323630836);
	insertIntoMap(root, "linkcount", 2);
	//used to identify if this inode is for a directory
	insertIntoMap(root, "filename_to_inode_dict", 0);
	insertIntoMap(root, "d.", ROOT_DIRECTORY_INODE);
	insertIntoMap(root, "d..", ROOT_DIRECTORY_INODE);
	for (int i = 0; i < 10000; i++) {
		createFile(i);
	}
	vector<int> tmpBlockList;
	generateNumList(27, 400, tmpBlockList);
	writeNumToBlock(1, tmpBlockList);
	for (int i = 2; i < 26; i++) {
		vector<int> tmp;
		generateNumList(400 * (i - 1), 400 * i, tmp);
		writeNumToBlock(i, tmp);
	}
	writeMaptoBlock(0, SUPER_BLOCK);
	writeMaptoBlock(26, root);
}

/***************************file system command functions***************************/
//STATFS
int statfs(const char* aPath, struct statvfs* aStatvfs) {
	string path = aPath;
	string truePath = getAbsolutePath(path);
	//should I delete this?
	if (pathToInodeMap.find(truePath) == pathToInodeMap.end()) {
		throw logic_error("Path doesn't exist");
	}
	aStatvfs->f_bsize = 4096;
	aStatvfs->f_blocks = 10000;
	int freeBlocks = 0;
	for (int i = 1; i < 26; i++) {
		vector<int> tmp = getBlockNumData(i);
		freeBlocks += tmp.size();
	}
	aStatvfs->f_bfree = freeBlocks;
	aStatvfs->f_bavail = freeBlocks;
	aStatvfs->f_files = pathToInodeMap.size() + 1; //plus root inode
	aStatvfs->f_namemax = 255;
	aStatvfs->f_ffree = 0;
	return 0;
}

//MKDIR
int fusekit::entry::mkdir(const char* aPath, mode_t mode) {
	string path = aPath;
	if (path == "") {
		throw logic_error("Path doesn't exist");
	}
	string truePath = getAbsolutePath(path);
	if (pathToInodeMap.find(truePath) != pathToInodeMap.end()) {
		throw logic_error("Path already exists");
	}
	string trueParentPath = getAbsParentPath(path);
	if (pathToInodeMap.find(trueParentPath) == pathToInodeMap.end()) {
		throw logic_error("Parent path doesn't exist");
	}
	int parentInode = pathToInodeMap[trueParentPath];
	map<string, int> parentInfo = getBlockInfoData(parentInode);
	if (parentInfo.find("filename_to_inode_dict") == parentInfo.end()) {
		throw logic_error("Parent path is not a directory");
	}
	string dirName = splitString(truePath, '/').back();
	int dirBlock = findFreeBlock();
	map<string, int> newInodeEntry;
	insertIntoMap(newInodeEntry, "size", 0);
	insertIntoMap(newInodeEntry, "uid", DEFAULT_UID);
	insertIntoMap(newInodeEntry, "gid", DEFAULT_GID);
	insertIntoMap(newInodeEntry, "mode", mode | S_IFDIR);
	insertIntoMap(newInodeEntry, "atime", 1323630836);
	insertIntoMap(newInodeEntry, "ctime", 1323630836);
	insertIntoMap(newInodeEntry, "mtime", 1323630836);
	insertIntoMap(newInodeEntry, "linkcount", 2);
	insertIntoMap(newInodeEntry, "filename_to_inode_dict", 0);
	insertIntoMap(newInodeEntry, "d.", dirBlock);
	insertIntoMap(newInodeEntry, "d..", parentInode);
	writeMaptoBlock(dirBlock, newInodeEntry);
	insertIntoMap(parentInfo, "d" + dirName, dirBlock);
	insertIntoMap(parentInfo, "linkcount", parentInfo["linkcount"] + 1);
	writeMaptoBlock(parentInode, parentInfo);
	insertIntoMap(pathToInodeMap, truePath, dirBlock);
	return 0;
}

//LINK
int link(const char* oldP, const char* newP) {
	string oldPath = oldP;
	string newPath = newP;
	string trueOldPath = getAbsolutePath(oldPath);
	if (pathToInodeMap.find(trueOldPath) == pathToInodeMap.end()) {
		throw logic_error("Old path doesn't exist");
	}
	int oldInode = pathToInodeMap[trueOldPath];
	if (S_ISDIR(getBlockInfoData(oldInode)["mode"])) {
		throw logic_error("Old path is a directory");
	}
	if (newPath == "") {
		throw logic_error("New path doesn't exist");
	}
	string trueNewPath = getAbsolutePath(newPath);
	if (pathToInodeMap.find(trueNewPath) != pathToInodeMap.end()) {
		throw logic_error("New path already exists");
	}
	string trueNewParentPath = getAbsParentPath(newPath);
	if (pathToInodeMap.find(trueNewParentPath) == pathToInodeMap.end()) {
		throw logic_error("New path doesn't exist");
	}
	int newParentInode = pathToInodeMap[trueNewParentPath];
	if (!S_ISDIR(getBlockInfoData(newParentInode)["mode"])) {
		throw logic_error("New path's parent is not a directory");
	}
	string newFileName = splitString(trueNewPath, '/').back();
	map<string, int> oldFileInode = getBlockInfoData(oldInode);
	insertIntoMap(oldFileInode, "linkcount", oldFileInode["linkcount"] + 1);
	writeMaptoBlock(oldInode, oldFileInode);
	map<string, int> pInode = getBlockInfoData(newParentInode);
	insertIntoMap(pInode, "f" + newFileName, oldInode);
	insertIntoMap(pInode, "linkcount", pInode["linkcount"] + 1);
	writeMaptoBlock(newParentInode, pInode);
	insertIntoMap(pathToInodeMap, trueNewPath, oldInode);
	return 0;
}

//UNLINK
int fusekit::entry::unlink(const char* path) {
	string truePath = getAbsolutePath(path);
	if (pathToInodeMap.find(truePath) == pathToInodeMap.end()) {
		throw logic_error("Path doesn't exist");
	}
	int thisInode = pathToInodeMap[truePath];
	if (S_ISDIR(getBlockInfoData(thisInode)["mode"])) {
		throw logic_error("Path is a directory");
	}
	string trueParentPath = getAbsParentPath(path);
	int parentInode = pathToInodeMap[trueParentPath];
	string dirName = splitString(truePath, '/').back();
	map<string, int> pInode = getBlockInfoData(parentInode);
	deleteFromMap(pInode, "f" + dirName);
	insertIntoMap(pInode, "linkcount", pInode["linkcount"] - 1);
	writeMaptoBlock(parentInode, pInode);
	deleteFromMap(pathToInodeMap, truePath);
	map<string, int> fileInode = getBlockInfoData(thisInode);
	insertIntoMap(fileInode, "linkcount", fileInode["linkcount"] - 1);
	if (fileInode["linkcount"] > 0) {
		writeMaptoBlock(thisInode, fileInode);
	} else {
		addOneToFreeBlock(thisInode);
		if (fileInode["indirect"] == 0) {
			addOneToFreeBlock(fileInode["location"]);
		} else {
			addOneToFreeBlock(fileInode["location"]);
			vector<int> blockList = getBlockNumData(fileInode["location"]);
			addToFreeBlock(blockList);
		}
	}
	return 0;
}

//CREATE
int create(const char* pathName, mode_t mode,
		struct fuse_file_info* fuseFileInfo) {
	try {
		string path = pathName;
		if (path == "") {
			throw logic_error("Path doesn't exist");
		}
		string truePath = getAbsolutePath(path);
		map<string, int> filePosition;
		if (pathToInodeMap.find(truePath) == pathToInodeMap.end()) {
			if (!(O_CREAT & fuseFileInfo->flags)) {
				throw logic_error("No creation");
			}
			string trueParentPath = getAbsParentPath(path);
			if (pathToInodeMap.find(trueParentPath) == pathToInodeMap.end()) {
				throw logic_error("Parent path doesn't exist");
			}
			int parentInode = pathToInodeMap[trueParentPath];
			if (S_ISDIR(getBlockInfoData(parentInode)["mode"])) {
				throw logic_error("Parent path is not a directory");
			}
			string fileName = splitString(truePath, '/').back();
			int iNodeBlock = findFreeBlock();
			int locationBlock = findFreeBlock();
			mode_t effectiveMode = S_IFREG | mode;
			map<string, int> newInodeEntry;
			insertIntoMap(newInodeEntry, "size", 0);
			insertIntoMap(newInodeEntry, "uid", DEFAULT_UID);
			insertIntoMap(newInodeEntry, "gid", DEFAULT_GID);
			insertIntoMap(newInodeEntry, "mode", effectiveMode);
			insertIntoMap(newInodeEntry, "atime", 1323630836);
			insertIntoMap(newInodeEntry, "ctime", 1323630836);
			insertIntoMap(newInodeEntry, "mtime", 1323630836);
			insertIntoMap(newInodeEntry, "linkcount", 1);
			insertIntoMap(newInodeEntry, "indirect", 0);
			insertIntoMap(newInodeEntry, "location", locationBlock);
			writeMaptoBlock(iNodeBlock, newInodeEntry);
			map<string, int> pInode = getBlockInfoData(parentInode);
			insertIntoMap(pInode, "f" + fileName, iNodeBlock);
			insertIntoMap(pInode, "linkcount", pInode["linkcount"] + 1);
			writeMaptoBlock(parentInode, pInode);
			insertIntoMap(pathToInodeMap, truePath, iNodeBlock);
			insertIntoMap(filePosition, "block", locationBlock);
			insertIntoMap(filePosition, "position", 0);
			insertIntoMap(fileInfoMap, iNodeBlock, filePosition);
		}
		return 0;
	} catch (exception &e) {
		throw logic_error("System call error");
		return 0;
	}
}

//OPEN
int open(const char* aPath, struct fuse_file_info *fuseFileInfo) {
	string path = aPath;
	if (path == "") {
		throw logic_error("Wrong path");
	}
	string truePath = getAbsolutePath(path);
	map<string, int> filePosition;
	bool recentCreated = false;
	//file doesn't exist, we create one here
	if (pathToInodeMap.find(truePath) == pathToInodeMap.end()) {
		create(aPath, S_IFREG, fuseFileInfo);
		recentCreated = true;
	} else {
		if ((O_CREAT & fuseFileInfo->flags) && (O_EXCL & fuseFileInfo->flags)) {
			throw logic_error("The file exists");
		}
		if (O_TRUNC & fuseFileInfo->flags) {
			//delete then open
			int iNode = pathToInodeMap[truePath];
			if (openedFileMap.find(iNode) != openedFileMap.end()) {
				//means close the file
				deleteFromMap(openedFileMap, iNode);
			}
			map<string, int> fileInode = getBlockInfoData(iNode);
			insertIntoMap(fileInode, "size", 0);
			if (fileInode["indirect"] == 0) {
				addOneToFreeBlock(fileInode["location"]);
			} else {
				addOneToFreeBlock(fileInode["location"]);
				vector<int> blockList = getBlockNumData(fileInode["location"]);
				addToFreeBlock(blockList);
			}
			writeMaptoBlock(iNode, fileInode);
			insertIntoMap(openedFileMap, iNode,
					getFileName(fileInode["location"]));
			insertIntoMap(filePosition, "block", iNode);
			insertIntoMap(filePosition, "position", 0);
		}
	}
	int iNode = pathToInodeMap[truePath];
	if (S_ISREG(getBlockInfoData(iNode)["mode"])) {
		if (openedFileMap.find(iNode) == openedFileMap.end()) {
			map<string, int> tmpInfo = getBlockInfoData(iNode);
			if (tmpInfo["indirect"] == 0) {
				//means open the file
				insertIntoMap(openedFileMap, iNode,
						getFileName(tmpInfo["location"]));
			} else {
				vector<int> blockList = getBlockNumData(tmpInfo["location"]);
				//actual start location of the content of the file
				insertIntoMap(openedFileMap, iNode, getFileName(blockList[0]));
			}
		}
		insertIntoMap(filePosition, "block", iNode);
		insertIntoMap(filePosition, "position", 0);
	}
	//if we just need to append something to the file
	else if (O_APPEND & fuseFileInfo->flags) {
		map<string, int> tmpInfo = getBlockInfoData(iNode);
		if (tmpInfo["indirect"] == 0 && tmpInfo["size"] < 4096) {
			insertIntoMap(filePosition, "block", tmpInfo["location"]);
			//store position as the end of the file
			insertIntoMap(filePosition, "position", tmpInfo["size"]);
		} else {
			vector<int> blockList = getBlockNumData(tmpInfo["location"]);
			int blockNumber = blockList.back();
			insertIntoMap(filePosition, "block", blockNumber);
			//store position as the end of the file
			insertIntoMap(filePosition, "position", tmpInfo["size"] % 4096);
		}
	} else {
		insertIntoMap(filePosition, "block", iNode);
		insertIntoMap(filePosition, "position", 0);
	}
	map<string, int> tmpMap;
	if (!recentCreated) {
		insertIntoMap(tmpMap, "position", filePosition["position"]);
		insertIntoMap(tmpMap, "block", filePosition["block"]);
	}
	insertIntoMap(tmpMap, "inode", iNode);
	insertIntoMap(tmpMap, "flags", fuseFileInfo->flags & O_RDWR);
	insertIntoMap(fileInfoMap, iNode, tmpMap);
	return iNode;
}

//READ
int read(const char* aPath, char* result, size_t count, off_t offset,
		struct fuse_file_info *fuseFile) {
	string path = aPath;
	string truePath = getAbsolutePath(path);
	if (pathToInodeMap.find(truePath) == pathToInodeMap.end()) {
		throw logic_error("File doesn't exist");
	}
	int iNode = pathToInodeMap[truePath];
	if (fileInfoMap.find(iNode) == fileInfoMap.end()) {
		throw logic_error("File is not open");
	}
	if ((O_WRONLY & fileInfoMap[iNode]["flag"]) != 0) {
		throw logic_error("File is not open for read");
	}
	if (!S_ISREG(getBlockInfoData(iNode)["mode"])) {
		throw logic_error("File doesn't refer to a regular file");
	}
	string strData;
	int startPosition = fileInfoMap[iNode]["position"];
	startPosition += offset;
	int startBlockNumber = fileInfoMap[iNode]["block"];
	map<string, int> infoMap = getBlockInfoData(iNode);
	//file is in one block only
	if (infoMap["indirect"] == 0) {
		string fileName = openedFileMap[iNode];
		strData = readFile(fileName, startPosition, count);
		insertIntoMap(fileInfoMap[iNode], "position",
				fileInfoMap[iNode]["position"] + strData.size());
	} else {
		vector<int> blockList = getBlockNumData(infoMap["location"]);
		int start = *find(blockList.begin(), blockList.end(), startBlockNumber);
		//the content we want to read is in one file
		if ((int) count <= 4096 - startPosition) {
			strData = readFile(getFileName(startBlockNumber), startPosition,
					count);
			insertIntoMap(openedFileMap, iNode, getFileName(startBlockNumber));
			insertIntoMap(fileInfoMap[iNode], "position",
					fileInfoMap[iNode]["position"] + strData.size());
		} else {
			//read the reamining content of this file first
			strData = readFile(getFileName(startBlockNumber), startPosition,
					count);
			//if read can be done in next one file
			if ((count - 4096 + startPosition) / 4096 == 0) {
				strData += readFile(getFileName(blockList[start + 1]), 0,
						count - 4096 + startPosition % 4096);
				insertIntoMap(openedFileMap, iNode,
						getFileName(blockList[start + 1]));
				insertIntoMap(fileInfoMap[iNode], "position",
						count - 4096 + startPosition);
				insertIntoMap(fileInfoMap[iNode], "block",
						blockList[start + 1]);
			} else {
				int i = 0;
				for (i = 0; i < ((int) count - 4096 + startPosition) / 4096;
						i++) {
					strData += readAllFile(
							getFileName(blockList[start + 1 + i]));
				}
				strData += readFile(getFileName(blockList[start + 2 + i]), 0,
						(count - 4096 + startPosition) % 4096);
				insertIntoMap(openedFileMap, iNode,
						getFileName(blockList[start + 2 + i]));
				insertIntoMap(fileInfoMap[iNode], "position",
						(count - 4096 + startPosition) % 4096);
				insertIntoMap(fileInfoMap[iNode], "block",
						blockList[start + 2 + i]);
			}
		}
	}
	//string to char*
	vector<char> v(strData.begin(), strData.end());
	result = &v[0];
	return 0;
}

//WRITE
int write(const char* aPath, const char* content, size_t count, off_t offset,
		struct fuse_file_info *fuseFileInfo) {
	string path = aPath;
	string truePath = getAbsolutePath(path);
	string data = content;
	if (pathToInodeMap.find(truePath) == pathToInodeMap.end()) {
		throw logic_error("File doesn't exist");
	}
	int iNode = pathToInodeMap[truePath];
	if (openedFileMap.find(iNode) == openedFileMap.end()) {
		throw logic_error("File is not open");
	}
	if ((O_RDONLY & fileInfoMap[iNode]["flag"]) != 0) {
		throw logic_error("File is not open for writing");
	}
	map<string, int> infoMap = getBlockInfoData(iNode);
	if (!S_ISREG(getBlockInfoData(iNode)["mode"])) {
		throw logic_error("File doesn't refer to a regular file");
	}
	int position = fileInfoMap[iNode]["position"];
	position += offset;
	int blockNumber = fileInfoMap[iNode]["block"];
	//file is stored in many blocks
	if (infoMap["indirect"] != 0) {
		vector<int> blockList = getBlockNumData(infoMap["location"]);
		position = (*find(blockList.begin(), blockList.end(), blockNumber))
				* 4096 + position;
	}
	int fileSize = infoMap["size"];
	//needs to fill with 0s
	int blankByteCount = position - fileSize;
	if (blankByteCount > 0) {
		string newData;
		for (int i = 0; i < blankByteCount; i++) {
			newData += "0";
		}
		newData += data;
		//can be handled in one block
		if ((int) newData.size() <= 4096 - fileSize % 4096) {
			if (infoMap["indirect"] == 0) {
				writeFile(getFileName(infoMap["location"]), fileSize, newData);
			} else {
				vector<int> blockList = getBlockNumData(infoMap["location"]);
				writeFile(getFileName(blockList[position / 4096]),
						fileSize % 4096, newData);
			}
		} else {
			//original file is in one block, but new data cannont be handled in one block
			if (infoMap["indirect"] == 0) {
				writeFile(getFileName(infoMap["location"]), fileSize,
						newData.substr(0, 4096 - fileSize));
				newData = newData.substr(4096 - fileSize, newData.size());
				vector<int> blockList = findMultiFreeBlocks(
						newData.size() / 4096);
				for (vector<int>::size_type i = 0; i < blockList.size(); i++) {
					writeFile(getFileName(blockList[i]), 0,
							newData.substr(0, 4096));
					newData = newData.substr(4096, newData.size());
				}
				int freeBlock = findFreeBlock();
				writeFile(getFileName(freeBlock), 0, newData);
			}
			//original file is in multiple blocks
			else {
				vector<int> blockList = getBlockNumData(infoMap["location"]);
				writeFile(getFileName(blockList[position / 4096]),
						fileSize % 4096,
						newData.substr(0, 4096 - fileSize % 4096));
				newData = newData.substr(4096 - fileSize % 4096,
						newData.size());
				vector<int> blockList2 = findMultiFreeBlocks(
						newData.size() / 4096);
				for (int i = 0; i < (int) blockList2.size(); i++) {
					writeFile(getFileName(blockList2[i]), 0,
							newData.substr(0, 4096));
					newData = newData.substr(0, 4096);
				}
				writeFile(getFileName(findFreeBlock()), 0, newData);
			}
		}
	}
	//doesn't need to add extra 0s
	else {
		//can be handled within one file
		if ((int) data.size() <= fileSize - position) {
			//original file is stored in one block
			if (infoMap["indirect"] == 0) {
				writeFile(getFileName(infoMap["location"]), position, data);
				insertIntoMap(fileInfoMap[iNode], "position",
						fileInfoMap[iNode]["position"] + data.size());
			} else {
				vector<int> blockList = getBlockNumData(infoMap["location"]);
				//can be written to one block
				if ((int) data.size() <= 4096 - position % 4096) {
					writeFile(getFileName(blockList[position / 4096]),
							position % 4096, data);
					insertIntoMap(fileInfoMap[iNode], "position",
							fileInfoMap[iNode]["position"] + data.size());
				} else {
					writeFile(getFileName(blockList[position / 4096]),
							position % 4096,
							data.substr(0, 4096 - position % 4096));
					string newData = data.substr(4096 - position % 4096,
							data.size());
					int num = newData.size() / 4096;
					for (int i = 0; i < num; i++) {
						writeFile(
								getFileName(blockList[position / 4096 + 1 + i]),
								0, newData.substr(0, 4096));
						newData = newData.substr(4096, newData.size());
					}
					writeFile(getFileName(blockList[position / 4096 + 1 + num]),
							0, newData);
					insertIntoMap(fileInfoMap[iNode], "position",
							newData.size());
					insertIntoMap(fileInfoMap[iNode], "block",
							blockList[position / 4096 + 1 + num]);
				}
			}
		} else {
			string newData = data.substr(0, fileSize - position);
			//oiginal file is in one block
			if (infoMap["indirect"] == 0) {
				char tmp[10];
				sprintf(tmp, "%d", infoMap["location"]);
				writeFile(getFileName(infoMap["location"]), position, newData);
				newData = data.substr(fileSize - position, data.size());
				vector<int> blockList = findMultiFreeBlocks(
						newData.size() / 4096);
				for (int i = 0; i < (int) blockList.size(); i++) {
					writeFile(getFileName(blockList[i]), 0,
							newData.substr(0, 4096));
					newData = newData.substr(4096, newData.size());
				}
				int fblock = findFreeBlock();
				writeFile(getFileName(fblock), 0, newData);
				insertIntoMap(fileInfoMap[iNode], "position", newData.size());
				insertIntoMap(fileInfoMap[iNode], "block", fblock);
			} else {
				vector<int> blockList = getBlockNumData(infoMap["location"]);
				writeFile(getFileName(blockList[position / 4096]),
						position % 4096,
						data.substr(0, 4096 - position % 4096));
				string newData = data.substr(4096 - position % 4096,
						data.size());
				for (int i = position / 4096 + 1;
						i < (int) blockList.size() - 1; i++) {
					writeFile(getFileName(blockList[i]), position % 4096,
							newData.substr(0, 4096));
					newData = newData.substr(4096, newData.size());
				}
				writeFile(getFileName(blockList.back()), 0,
						newData.substr(0, fileSize % 4096));
				newData = newData.substr(fileSize % 4096, newData.size());
				if ((int) newData.size() - fileSize + position
						< 4096 - fileSize % 4096) {
					writeFile(getFileName(blockList.back()), fileSize % 4096,
							newData);
					insertIntoMap(fileInfoMap[iNode], "position",
							fileInfoMap[iNode]["position"] + newData.size());
				} else {
					writeFile(getFileName(blockList.back()), fileSize % 4096,
							newData.substr(0, 4096 - fileSize % 4096));
					newData = newData.substr(4096 - fileSize % 4096,
							newData.size());
					vector<int> blockList2 = findMultiFreeBlocks(
							newData.size() / 4096);
					for (int i = 0; i < (int) blockList2.size(); i++) {
						writeFile(getFileName(blockList2[i]), 0,
								newData.substr(0, 4096));
						newData = newData.substr(4096, newData.size());
					}
					int freeBlock = findFreeBlock();
					writeFile(getFileName(freeBlock), 0, newData);
					insertIntoMap(fileInfoMap[iNode], "position",
							newData.size());
					insertIntoMap(fileInfoMap[iNode], "block", freeBlock);
					vector<int> indexList = getBlockNumData(
							infoMap["location"]);
					indexList.insert(indexList.end(), blockList2.begin(),
							blockList2.end());
					indexList.push_back(freeBlock);
					writeNumToBlock(infoMap["location"], indexList);
				}
			}
		}
	}
	return data.size();
}

//OPEN DIR
int opendir(const char* aPath, struct fuse_file_info *fuseFileInfo) {
	string path = aPath;
	if (path == "") {
		throw logic_error("The dir doesn't exist");
	}
	string truePath = getAbsolutePath(path);
	map<string, int> dirPosition;
	if (pathToInodeMap.find(truePath) == pathToInodeMap.end()) {
		if (!(O_CREAT & fuseFileInfo->flags)) {
			throw logic_error("Cannot create directory");
		}
		mkdir(aPath, fuseFileInfo->flags);
	}
	int iNode = pathToInodeMap[truePath];
	if (!S_ISDIR(getBlockInfoData(iNode)["mode"])) {
		throw logic_error("It's not a directory");
	}
	insertIntoMap(openedFileMap, iNode, getFileName(iNode));
	return iNode;
}

//RELEASE
int release(const char* aPath, struct fuse_file_info *fuseFile) {
	string path = aPath;
	string truePath = getAbsolutePath(path);
	int iNode = pathToInodeMap[truePath];
	deleteFromMap(openedFileMap, iNode);
	return 0;
}

//RELEASE DIR
int releasedir(const char* aPath, struct fuse_file_info *fuseFile) {
	string path = aPath;
	string truePath = getAbsolutePath(path);
	int iNode = pathToInodeMap[truePath];
	deleteFromMap(openedFileMap, iNode);
	return 0;
}

//READ DIR
int readdir(const char* aPath, void* buf, fuse_fill_dir_t fuseFiller,
		off_t offset, struct fuse_file_info *fuseFileInfo) {
	string path = aPath;
	string truePath = getAbsolutePath(path);
	if (pathToInodeMap.find(truePath) == pathToInodeMap.end()) {
		throw logic_error("Directory doesn't exist");
	}
	int iNode = pathToInodeMap[truePath];
	if (openedFileMap.find(iNode) == openedFileMap.end()) {
		throw logic_error("Directory is not open");
	}
	if (S_ISDIR(getBlockInfoData(iNode)["mode"])) {
		throw logic_error("Not a directory");
	}
	vector<string> result;
	map<string, int> infoMap = getBlockInfoData(iNode);
	for (map<string, int>::iterator it = infoMap.begin(); it != infoMap.end();
			++it) {
		if (find(nonInodeInfoKeys.begin(), nonInodeInfoKeys.end(), it->first)
				== nonInodeInfoKeys.end()) {
			fuseFiller(buf, (it->first).c_str(), NULL, 0);
		}
	}
	return 0;
}

//READ LINK
ssize_t readlink(const char* aPath, char *res, size_t count) {
	string path = aPath;
	if (path == "") {
		throw logic_error("Wrong path");
	}
	string truePath = getAbsolutePath(path);
	if (pathToInodeMap.find(truePath) == pathToInodeMap.end()) {
		throw logic_error("The file does not exist.");
	}
	int iNode = pathToInodeMap[truePath];
	map<string, int> fileInode = getBlockInfoData(iNode);
	struct fuse_file_info *fuseFileInfo = { O_RDONLY };
	read(aPath, res, count, 0, fuseFileInfo);
	return 0;
}

//GET ATTR
int getattr(const char* aPath, struct stat *res) {
	string path = aPath;
	string truePath = getAbsolutePath(path);
	if (pathToInodeMap.find(truePath) == pathToInodeMap.end()) {
		throw logic_error("The file doesn't exist");
	}
	int iNode = pathToInodeMap[truePath];
	map<string, int> info = getBlockInfoData(iNode);
	res->st_mode = info["mode"];
	res->st_ino = iNode;
	res->st_nlink = info["linkcount"];
	res->st_rdev = 0;
	res->st_uid = info["uid"];
	res->st_gid = info["gid"];
	res->st_size = info["size"];
	res->st_atime = info["atime"];
	res->st_mtime = info["mtime"];
	res->st_ctime = info["ctime"];
	res->st_blocks = info["indirect"];
	return 0;
}

//RENAME
int rename(const char* oldPath, const char* newPath) {
	string trueOldPath = getAbsolutePath(oldPath);
	string trueNewPath = getAbsolutePath(newPath);
	if (pathToInodeMap.find(trueOldPath) == pathToInodeMap.end()) {
		throw logic_error("The old file doesn't exist.");
	}
	if (pathToInodeMap.find(trueNewPath) == pathToInodeMap.end()) {
		throw logic_error("The new file already exists");
	}
	if (trueNewPath == "") {
		throw logic_error("name error");
	}
	string trueParentPathOld = getAbsParentPath(trueOldPath);
	int parentInode = pathToInodeMap[trueParentPathOld];
	map<string, int> pInode = getBlockInfoData(parentInode);
	int iNode = pathToInodeMap[trueOldPath];
	string newName = splitString(trueNewPath, '/').back();
	insertIntoMap(pInode, "f" + newName, iNode);
	insertIntoMap(pathToInodeMap, trueNewPath, iNode);
	string oldName = splitString(trueOldPath, '/').back();
	deleteFromMap(pInode, "f" + oldName);
	deleteFromMap(pathToInodeMap, trueOldPath);
	writeMaptoBlock(parentInode, pInode);
	return 0;
}

//DESTROY
void destroy() {
	nonInodeInfoKeys.clear();
	SUPER_BLOCK.clear();
	pathToInodeMap.clear();
	fileInfoMap.clear();
	//should close all files
	openedFileMap.clear();
	fileSysCallContext.clear();
	root.clear();
	removeAllFile();
}
int main(int argc, char *argv[]) {
	fusekit::daemon<> &daemon = fusekit::daemon<>::instance();

	return daemon.run(argc, argv);
}

