//版本5.2
//3.6改良，先遍历所有函数，在动态组合
//4.0改良，主要是减少插装函数内容，所有事可以放到finalize中做，也可以输出日志。
//5.0 改良，静态获取程序结构树，动态得到间接调用关系和采样数据，分析得到完整的树，并加上计算节点，用SAMPLE结果进行时间分析
//5.1 如果按照函数栈搜索时遇到CALL_REC等，直接返回节点，并把函数栈清空。
//	函数栈如果在中途遇到一些节点先退出了，要把函数栈清空
//	CALL LOG太多，间接调用重复太多
//	加COMPUTING节点时，有重复现象，提前加以判断
//	多次遇到相同的间接调用时，只进行一次扩展拼接，提前加以判断
//	该版本动态拼接函数时，未拷贝copy一棵树，即所有调用该函数的地方都会累计起来。

//5.2 改良
//      1. ENTRY和EXIT的开始和结束进行PAPI_stop和 PAPI_start， 不统计插装的执行的cycle数，
//              BUG：解决FORTRAN的mainstart（调用main函数的函数）在init之前papi_stop的问题，判断不对该函数插装。
//      2. 增加了计时，统计非写入LOG的时间
//      3. 采用unw_backtrace的方法获取函数调用栈
//      4. LOOP的行号范围重新确定，代码解释IR的意思没有问题，但是IR中preheader指向的行号不对，应该是preheader指向的BB中跳转指令的行号。
#include "IRStruct.h"
#include <vector>
#include <iostream>
#include <fstream>
#include <cassert>
#include <chrono>
#include <stack>
#include <algorithm>
#include <vector>
#include <queue>
#include <string>
#include <mpi.h>

using namespace std;

//#define DEBUG_PRINT
#define MAX_CALL_STACK 100


class Node {
	public:
		int id = -1;
		int type = -10;
		int firstType = -10;
		int lastType = -10;
		int numChildren = 0;
		int dirID = -1;
		int fileID = -1;
		int lineNum = -1;
		int exitLineNum = -1;
		Node* parent = nullptr;
		vector<Node*> children;

		int sampleCount = 0;
		double sumTime = 0;

		bool comNodeExpanded = false;

		Node() {}

		Node(int id, int type, int firstType, int lastType, int numChildren, int dirID, int fileID, int lineNum, int exitLineNum):
			id(id), type(type), firstType(firstType), lastType(lastType), numChildren(numChildren), dirID(dirID), fileID(fileID), lineNum(lineNum), exitLineNum(exitLineNum){
				children.resize(static_cast<unsigned int>(numChildren));
			}

};

typedef struct logstruct{
	char funcType = 0; // Y for entry , T for exit , L for LATCH , C for COMBINE
	int childId = 0;
	int id = 0;
	int nodeType = 0; // 
	long long time = 0;
}LOG;


LOG logline;


//int mpiRank;

int maxId = 0;

static Node* root = nullptr;
static Node* now = nullptr;
static Node* last = nullptr;
static vector<Node*> trees;
static vector<string> dirs, files;
static stack<string> lineStack;
static string popline;

static void readTree(Node*& node, istream& in, unsigned int depth) {
	int id, type, firstType, lastType, numChildren, dirID, fileID, lineNum, exitLineNum;
	in >> id >> type >> firstType >> lastType >> numChildren >> dirID >> fileID >> lineNum >> exitLineNum;
	node = new Node(id, type, firstType, lastType, numChildren, dirID, fileID, lineNum, exitLineNum);
	maxId = ( maxId > id ) ? maxId : id ;
	for (auto& child: node->children) {
		readTree(child, in, depth + 1);
		child->parent = node;
	}
}
static Node* copyTree(Node* node){

	//copy node to newnode
	Node* newnode = new Node(node->id, node->type, node->firstType, node->lastType, node->numChildren, node->dirID, node->fileID, node->lineNum, node->exitLineNum);
	//copy child to newnode
	int i = 0;
	for (auto child: node->children) {
		Node* newchild = copyTree(child);
		newnode->children[i] = newchild;
		newchild->parent = newnode;     
		i++;   
	}
	return newnode;

}

static Node* findTreeRootWithId(int id){
	for(auto& node: trees){
		if(node->id == id)
			return node;
	}

}

static Node* findCallIndirectNodeWithId(Node* node, int id){

	if(id == node->id && node->type == CALL_INDIRECT){
		return node;
	}

	for (auto child: node->children){
		Node* retNode = findCallIndirectNodeWithId(child, id);
		if(retNode){
			return retNode;
		}
	}

	return nullptr;
}

static void writeTree(Node* node, ostream& out) {
	out << node->id << " "
		<< node->type << " "
		<< node->children.size() << " "
		<< node->dirID << " "
		<< node->fileID << " "
		<< node->lineNum << " "
		<< node->exitLineNum << " "
		<< node->sampleCount << " "
		<< node->sumTime << " "
		<< "\n";
	node->sampleCount = 0;
	node->sumTime = 0;
	for (auto child: node->children)
		writeTree(child, out);

}

static void initialize() {
	ifstream inputStream("out.txt");
	if (!inputStream.good()) {
		cerr << "Failed to open out.txt\n";
		exit(1);
	}
	int numTrees = 0;
	inputStream >> numTrees;
	//cerr <<"before read tree!\n";
	int totalMaxDepth = 0;
	for(int i = 0;i < numTrees; i++){
		Node *treeRoot = nullptr;
		readTree(treeRoot, inputStream, 0);
		trees.push_back(treeRoot);
	}
	root = trees[0];
	unsigned int cnt = 0;
	inputStream >> cnt;
	dirs.resize(cnt);
	for (int i = 0; i < cnt; ++i)
		inputStream >> dirs[i];
	inputStream >> cnt;
	files.resize(cnt);
	for (int i = 0; i < cnt; ++i)
		inputStream >> files[i];
}


static void finalize(int mpiRank) {
	cerr<< "ENTER FINALIZE"<<endl;
	ofstream outputStream(string("stat") + to_string(mpiRank) + string(".txt"));
	if (!outputStream.good()) {
		cerr << "Failed to open output file"<<endl;
		return;
	}
	writeTree(root, outputStream);
	outputStream << dirs.size() << "\n";
	for (auto& s: dirs)
		outputStream << s << "\n";
	outputStream << files.size() << "\n";
	for (auto& s: files)
		outputStream << s << "\n";

	return ;
}



static void debug_print(Node* now, int type, int id, string funcName) {
#ifdef DEBUG_PRINT
	if (now && now->dirID >= 0)
		cerr << funcName << "\t" << type << "\t" << id << "\t" << dirs[now->dirID] << "/" << files[now->fileID] << ":" << now->lineNum << endl;
	else
		cerr << funcName << "\t" << type << "\t" << id << "\n";
#endif
}


void entryHandler(int id, int type) {
	if (type == CALL_INDIRECT){
		//cerr<<"CALL_INDIRECT\n";
		now = findCallIndirectNodeWithId(root, id);
		debug_print(now, type, id, __func__);
		return;
	}

	if(type == FUNCTION && now && now->type == CALL_INDIRECT){ // exclude main function
		for(auto child : now->children){
			if(child->id == id){
				return ;
			}
		}
		now -> numChildren ++;
		//Node * indirectFuncNode = copyTree(findTreeRootWithId(id));
		Node * indirectFuncNode = findTreeRootWithId(id);
		now -> children.push_back(indirectFuncNode);
		now -> numChildren = now -> children.size();
		indirectFuncNode -> parent = now;
		now = indirectFuncNode;
		debug_print(now, type, id, __func__);
		return ;
	}

	debug_print(now, type, id, __func__);

}

void exitHandler(int id, int type) {
	now = nullptr;
	debug_print(now, type, id, __func__);
}


void expandComputingNode(Node* node){
	if(node->comNodeExpanded){
		return;
	}
	//cerr<<__LINE__<<endl;
	int curLineNum = node-> lineNum;
	queue<Node*> tempComNodeQueue;
	//cerr <<endl << node->id <<" "<< node->type<<" " << node->lineNum<<" " <<node->exitLineNum<<endl;
	for(auto child: node-> children){
		//cerr<<__LINE__<<endl;
		if(child && child->type != COMPUTING && (node->type == FUNCTION ||node->type == LOOP)){ //only add computing node to function and loop node
			//cerr<<__LINE__<<endl;
			//cerr << child->id <<" "<< child->type<<" " << child->lineNum<<" " <<child->exitLineNum<<endl;
			if(child->lineNum > curLineNum){
				//cerr<<__LINE__<<endl;
				maxId ++;
				Node *comNode = new Node(maxId, COMPUTING, COMPUTING, COMPUTING, 0, node->dirID, node->fileID, curLineNum , child->lineNum - 1);
				tempComNodeQueue.push(comNode);
				//cerr << "push "<<comNode->id << " : "<< comNode->lineNum <<" - "<<comNode->exitLineNum<<endl;

			}
			//curLineNum = child->exitLineNum;
		}
		if(child && child->type != COMPUTING){
			//cerr<<__LINE__<<endl;
			expandComputingNode(child);
			//cerr<<__LINE__<<endl;
		}
		if(child)
			curLineNum = child->exitLineNum + 1;
	}
	if(node->type == FUNCTION ||node->type == LOOP){ //only add computing node to function and loop node
		//cerr<<__LINE__<<endl;
		if(node->exitLineNum > curLineNum){
			//cerr<<__LINE__<<endl;
			maxId ++;
			Node *comNode = new Node(maxId, COMPUTING, COMPUTING, COMPUTING, 0, node->dirID, node->fileID, curLineNum , node->exitLineNum );
			tempComNodeQueue.push(comNode);
			//cerr << "push "<< comNode->id << " : "<< comNode->lineNum <<" - "<<comNode->exitLineNum<<endl;
		}
	}

	while(!tempComNodeQueue.empty()){
		node->children.push_back(tempComNodeQueue.front());
		tempComNodeQueue.pop();
	}
	
	node->numChildren = node->children.size();
	node->comNodeExpanded = true;
}

void SplitString(const std::string& s, std::vector<std::string>& v, const std::string& c)
{
	std::string::size_type pos1, pos2;
	pos2 = s.find(c);
	pos1 = 0;
	while(std::string::npos != pos2)
	{
		v.push_back(s.substr(pos1, pos2-pos1));

		pos1 = pos2 + c.size();
		pos2 = s.find(c, pos1);
	}
	if(pos1 != s.length())
		v.push_back(s.substr(pos1));
}


Node* findNodeBFS(Node* node, int popFilesId, int popLineNum){

	//cerr<<"findNodeBFS: "<< node->id << endl;
	for(auto child : node->children){
		if(child -> fileID == popFilesId && (child->lineNum <= popLineNum && popLineNum <= child->exitLineNum) ){
			//cerr <<child->id << " : " <<child->type <<endl;
			if(child -> type >= 0 || child -> type == CALL ||child -> type == CALL_INDIRECT || child -> type == CALL_REC){
				//cerr << "return : "<< child->id << " type : " << child->type<<endl;
				return child;
			}else if(child -> type == LOOP || child->type == FUNCTION){
				return findNodeBFS(child,popFilesId,popLineNum);
			}else{
				//cerr << "return : "<< child->id << " type : " << child->type<<endl;
				return child;
			}
		}
	}

	for(auto child : node->children){
		return findNodeBFS(child,popFilesId,popLineNum);
	}

	//if node is leaf ,return nullptr
	//cerr << "return : NULL"<<endl;
	return nullptr;
}


Node* updateLeafWithLineStack(Node* node){
	//pop if popFlag is true
	if(lineStack.empty()){
		return node;
	}
	string popLine = lineStack.top();
	lineStack.pop();


	//find node -> popFlag = true

	vector<string> v;
	SplitString(popLine,v,":");

	auto it = find(files.begin(), files.end(), v[0]);
	if(it == files.end()){
		return nullptr;
	}
	int popFilesId = distance(  files.begin()  , it );

	int popLineNum = atoi(v[1].c_str());	

	//cerr<<"updateLeafWithLineStack: "<< node->id << " find fileid: "<<popFilesId << " line: "<<popLineNum<< endl;

	Node* findNode = findNodeBFS(node, popFilesId, popLineNum);


	if (!findNode){
		return nullptr;

	}
	//if(findNode->type == CALL_REC || findNode -> type >= 0 || findNode -> type == COMPUTING){
	if(!findNode->numChildren){
		return findNode;
	}

	return updateLeafWithLineStack(findNode);

}



void getLineStack(istream& in){

	string line;
	while(getline(in,line)){
		//cerr << line <<endl;
		if(line.find("??:") != string::npos){
			return;
		//}else if(line.find("??:?") != string::npos){
		}else{
			lineStack.push(line);
		}
		//vector<string> v;
		//SplitString(line,v,":");

		//cerr << v[1] <<endl;
	}
}


void updateLeafTimePercent(Node* node, int totalSampleCount){

	//if(node->numChildren == 0)
		node->sumTime = (double)node->sampleCount / (double)totalSampleCount ;
	for(auto child : node-> children){
		updateLeafTimePercent(child,totalSampleCount);
	}

}


int main(int argc, char**argv){
	initialize();

	//for(int i = 0;i < nprocs; i++){
	// read LOG
	ifstream inputStream((string("LOG0.TXT")));
	if (!inputStream.good()) {
		cerr << "Failed to open LOG.txt\n";
		exit(1);
	}	
	while(!inputStream.eof()){
		inputStream >> logline.funcType >> logline.id >> logline.nodeType ;

		switch(logline.funcType){
			case 'Y':
				entryHandler(logline.id,logline.nodeType);
				break;
			case 'T':
				exitHandler(logline.id,logline.nodeType);
				break;
		}
	}

	inputStream.close();

	expandComputingNode(root);

	int nprocs = atoi(argv[1]);
	int i = 0;
	while(i < nprocs){

		ifstream sampleStream((string("SAMPLE")+to_string(i) +string(".TXT-symb")));
		if (!sampleStream.good()) {
			cerr << "Failed to open sample.txt\n";
			exit(1);
		}


		int totalSampleCount = 0;
		while(!sampleStream.eof()){
			getLineStack(sampleStream);//push to linestack

			if( !lineStack.empty() ){
				Node* node = updateLeafWithLineStack(root);//pop from linstack
				if(node){
			//		cerr<<totalSampleCount<<" : "<< node-> id << endl;
					node->sampleCount++;	
				}
			//	else{cerr<<totalSampleCount<< " :" <<endl;}
				
				totalSampleCount++;
			}
	

			//如果函数栈因为遇到一些CALL_REC等节点类型，提前返回了，要把函数栈POP完。
			while(!lineStack.empty() ){
				lineStack.pop();
			}
	
			//totalSampleCount++;
		}

		sampleStream.close();

		updateLeafTimePercent(root,totalSampleCount);

		finalize(i);
		i++;
	}
	//}
}

