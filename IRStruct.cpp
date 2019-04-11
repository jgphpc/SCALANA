//5.2 version
//3.6 改良，先遍历所有函数，在动态组合
//3.7 去掉BRANCH，保留计算节点
//4.0改良，主要是减少插装函数内容，所有事可以放到finalize中做，也可以输出日志。
//5.0 改良，每个节点的代码行号范围，并且只有CALL——INDIRECT和未拼接的FUNCTION插装
//5.1 如果按照函数栈搜索时遇到CALL_REC等，直接返回节点，并把函数栈清空。
////      函数栈如果在中途遇到一些节点先退出了，要把函数栈清空
////      CALL LOG太多，间接调用重复太多
////      加COMPUTING节点时，有重复现象，提前加以判断
////      多次遇到相同的间接调用时，只进行一次扩展拼接，提前加以判断
////      该版本动态拼接函数时，未拷贝copy一棵树，即所有调用该函数的地方都会累计起来。
//
//5.2 改良
//	1. ENTRY和EXIT的开始和结束进行PAPI_stop和 PAPI_start， 不统计插装的执行的cycle数，
//		BUG：解决FORTRAN的mainstart（调用main函数的函数）在init之前papi_stop的问题，判断不对该函数插装。
//	2. 增加了计时，统计非写入LOG的时间
//	3. 采用unw_backtrace的方法获取函数调用栈
//	4. LOOP的行号范围重新确定，代码解释IR的意思没有问题，但是IR中preheader指向的行号不对，应该是preheader指向的BB中跳转指令的行号。
#include "llvm/Pass.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Analysis/CallGraphSCCPass.h"
#include "llvm/Analysis/LoopPass.h"
#include "llvm/Analysis/RegionPass.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/DebugInfo.h"
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <vector>
#include "IRStruct.h"

#define COMBINEFUNC

#define version36

using namespace std;
using namespace llvm;

namespace {

	class Node {
		public:
			int id = -1;
			int type;
			BasicBlock* bb;
			Function* func;
			CallInst* callInst;

			// For combine instrumentation
			int firstType = -10;
			int lastType = -10;
			int exitLineNum = -1;
			BasicBlock* exitBb = nullptr;
			CallInst* callInstPreheader;
			CallInst* callInstExit;

			// For loop/branch instrumentation
			BasicBlock* preheader = nullptr;
			BasicBlock* exit = nullptr;
			BasicBlock* latch = nullptr;

			bool expanded = false;
			bool trimmed = false;
			bool removed = false;
			bool compNodeExpanded = false;
			bool idGenerated = false;
			bool childIDGenerated = false;
			bool locationInfoCollected = false;
			bool instrumented = false;
#ifdef version36
			bool isAttachedToAnotherNode = false;
#endif
			// The index of this node in its parent's children array
			// Generated after trimming
			int childID = 0;
			int numChildren = 0;

			int dirID = -1;
			int fileID = -1;
			int lineNum = -1;

			vector<Node*> children;

			Node(NodeType type, BasicBlock* bb = nullptr, Function* func = nullptr, CallInst* callInst = nullptr):
				type(static_cast<int>(type)), bb(bb), func(func), callInst(callInst) {}

			Node(int funcId, BasicBlock* bb = nullptr, Function* func = nullptr, CallInst* callInst = nullptr):
				type(funcId), bb(bb), func(func), callInst(callInst) {}
	};

	static cl::opt<string> InputFilename(
			"if",
			cl::desc("Specify name of input file containing function names to instrument"),
			cl::value_desc("filename"),
			cl::Required
			);

	static cl::opt<string> OutputFilename(
			"of",
			cl::desc("Specify name of output file containing the program structure"),
			cl::value_desc("filename"),
			cl::Required
			);

	struct IRSPass : public ModulePass {
		static char ID;
#ifdef version36
		int maxNodeId = 0; // different init id for different trees
#endif
		unordered_map<Function* , Node*> func2node;
		unordered_set<Function*> recSet;
		queue<Function*> funcQue;
		unordered_map<string, int> instFuncMap;
		Constant* entryFunc = nullptr;
		Constant* exitFunc = nullptr;
		Constant* latchFunc = nullptr;
#ifdef COMBINEFUNC
		Constant* combineFunc = nullptr;
#endif
		vector<StringRef> dirs, files;

		IRSPass() : ModulePass(ID) {}

		void getAnalysisUsage(AnalysisUsage& AU) const override {
			AU.addRequired<CallGraph>();
			AU.addRequired<LoopInfo>();
			AU.addRequired<RegionInfo>();
		}

		bool runOnModule(Module& M) override {
			// Get functions to instrument
			ifstream inputStream(InputFilename.c_str());
			if (inputStream.good()) {
				int id;
				string s;
				while (inputStream >> id >> s) {
					instFuncMap[s] = id;

					// For fortran
					auto sf = s + '_';
					transform(s.begin(), s.end(), sf.begin(), ::tolower);
					instFuncMap[sf] = id;
				}
			} else {
				errs() << "Failed to open input file\n";
				return false;
			}

			// Mark all recursive functions
			unordered_map<CallGraphNode *, Function *> callGraphNodeMap;
			auto &cg = getAnalysis<CallGraph>();
			for (auto &f: M) {
				auto cgn = cg[&f];
				callGraphNodeMap[cgn] = &f;
#ifdef version36
				if (f.getName().startswith("llvm.") == false && f.getName().startswith("MPI_") == false && !f.isDeclaration()){
					funcQue.push(&f);
					func2node[&f] = nullptr;
				}
#endif
			}
			scc_iterator<CallGraph *> cgSccIter = scc_begin(&cg);
			while (!cgSccIter.isAtEnd()) {
				if (cgSccIter.hasLoop()) {
					const vector<CallGraphNode*>& nodeVec = *cgSccIter;
					for (auto cgn: nodeVec) {
						auto f = callGraphNodeMap[cgn];
						recSet.insert(f);
					}
				}
				++cgSccIter;
			}

			// Get functions to call from instrumentation points
			// Must be done after any use of CallGraph
			// because CallGraph will never contain the following inserted functions
			auto& context = M.getContext();
			entryFunc = M.getOrInsertFunction(
					ENTRY_FUNC_NAME,
					Type::getVoidTy(context),
					//Type::getInt32Ty(context),
					Type::getInt32Ty(context),
					Type::getInt32Ty(context),
					nullptr
					);
			exitFunc = M.getOrInsertFunction(
					EXIT_FUNC_NAME,
					Type::getVoidTy(context),
					//Type::getInt32Ty(context),
					Type::getInt32Ty(context),
					Type::getInt32Ty(context),
					nullptr
					);
			/*latchFunc = M.getOrInsertFunction(
			  LATCH_FUNC_NAME,
			  Type::getVoidTy(context),
			//Type::getInt32Ty(context),
			Type::getInt32Ty(context),
			Type::getInt32Ty(context),
			nullptr
			);*/

#ifdef COMBINEFUNC
			/*combineFunc = M.getOrInsertFunction(
			  COMBINE_FUNC_NAME,
			  Type::getVoidTy(context),
			//Type::getInt32Ty(context),
			Type::getInt32Ty(context),
			Type::getInt32Ty(context),
			nullptr
			);*/
#endif
			// Avoid recursion because getAnalysis has side effects
			auto mainFunc = M.getFunction("main");
#ifdef version36
			//errs()<<"-------------------\n";
			//printFunc2Node();
			//errs()<<"-------------------\n";
			while (!funcQue.empty()) {
				auto func = funcQue.front();
				//errs()<<func->getName()<<"\n";
				funcQue.pop();
				func2node[func] = buildTree(func);
			}
			//errs()<<"Has build Tree.\n";
			//auto root = func2node[mainFunc];
			maxNodeId = 0;
			for (auto& root : func2node){

				expand(root.second);
				//trim(root.second);
			}
			for (auto& root : func2node){
				if(root.second && !root.second->isAttachedToAnotherNode){
					trim(root.second);
				}
			}
			//printFunc2Node();
			int j =0;
			for (auto& root : func2node){
				if(root.second && !root.second->isAttachedToAnotherNode){
					//errs()<< j++ <<": "<<root.first->getName()<<"\n";
					//compNodeExpand(root.second);
				}
			}
			for (auto& root : func2node){
				if(root.second && !root.second->isAttachedToAnotherNode){
					generateID(root.second, maxNodeId);
					generateChildID(root.second);
				}
			}
			for (auto& root : func2node){
				if(root.second && !root.second->isAttachedToAnotherNode){
					collectLocationInfo(root.second);
				}
			}
			instrumentMain(func2node[mainFunc]);
			for (auto& root : func2node){
				if(root.first->getName() != "main" && root.second && !root.second->isAttachedToAnotherNode){
					instrument(root.second);
				}
			}
			//printFunc2Node();
#else
			funcQue.push(mainFunc);
			func2node[mainFunc] = nullptr;
			while (!funcQue.empty()) {
				auto func = funcQue.front();
				funcQue.pop();
				func2node[func] = buildTree(func);
			}

			auto root = func2node[mainFunc];

			expand(root);
			trim(root);
			compNodeExpand(root);
			generateID(root, 0);
			generateChildID(root);
			collectLocationInfo(root);
			instrument(root);
			//printTree(root, 0);
			//printFunc2Node();
#endif
			// Write tree to a file
			ofstream outputStream(OutputFilename.c_str());
			if (!outputStream.good()) {
				errs() << "Failed to open output file\n";
				return true;
			}
#ifdef version36
			int numTrees = 0;
			for (auto& root : func2node){
				if (root.second && !root.second->isAttachedToAnotherNode){
					if (!root.second->removed ){
						numTrees++;
					}
				}
			}
			//numTrees = 1;
			outputStream << numTrees <<"\n";
			writeTree(func2node[mainFunc], outputStream);
			for (auto& root : func2node){
				if(root.first->getName() != "main"){
					if (root.second && !root.second->isAttachedToAnotherNode){
						writeTree(root.second, outputStream);
					}
				}
			}

#else
			writeTree(root, outputStream);
#endif
			outputStream << dirs.size() << "\n";
			for (auto& s: dirs)
				outputStream << s.str() << "\n";
			outputStream << files.size() << "\n";
			for (auto& s: files)
				outputStream << s.str() << "\n";

			return true;
		}

		/*void writeTree(Node* node, ostream& out) {
		  if (!node || node->removed)
		  return;

		  out << node->id << " "
		  << node->type << " "
		  << node->numChildren << " "
		  << node->dirID << " "
		  << node->fileID << " "
		  << node->lineNum << "\n";
		  for (auto child: node->children)
		  writeTree(child, out);
		  }*/
		void writeTree(Node* node, ostream& out) {
			if (!node || node->removed)
				return;
			if (node -> type == COMBINE){
				out << node->id << " "
					<< node->type << " "
					<< node->firstType << " "
					<< node->lastType << " "
					<< node->numChildren << " "
					<< node->dirID << " "
					<< node->fileID << " "
					<< node->lineNum << " "
					<< node->exitLineNum << "\n";
			}else{
				out << node->id << " "
					<< node->type << " "
					<< node->type << " "
					<< node->type << " "
					<< node->numChildren << " "
					<< node->dirID << " "
					<< node->fileID << " "
					<< node->lineNum << " "
					<< node->exitLineNum << "\n";
				for (auto child: node->children)
					writeTree(child, out);
			}
			//for (auto child: node->children)
			//	writeTree(child, out);
		}
		void printTree(Node* node, int depth) {
			//if (node && node->removed )
			//	return;
			//if (node->isAttachedToAnotherNode)
			//	return;
			for (int i = 0; i < depth; ++i) {
				errs() << "  ";
			}
			if (!node) {
				errs() << "null\n";
				return;
			}
			errs() << node->id << "\t"
				<< node->type << "\t"
				<< node->childID << "\t"
				<< node->dirID << ":"
				<< node->fileID << ":"
				<< node->lineNum << "\n";
			for (auto child: node->children) {
				printTree(child, depth + 1);
			}
		}
#ifdef version36
		void printFunc2Node(){
			int i = 1;
			for (auto& iter : func2node){
				if (iter.second && !iter.second->isAttachedToAnotherNode){
					if (!iter.second->removed ){
						errs()<< i++ <<": "<<iter.first->getName()<<"\n";
						printTree(iter.second,0);
					}
				}
			}

		}
#endif

		void instrument(Node* node) {
#ifdef COMBINEFUNC
			if (!node || node->removed || node->instrumented)
#else
				if (!node || node->removed || node->instrumented || node->type == COMBINE)
#endif
					return;

			// This step must be post-order!!!

			auto& context = node->bb->getParent()->getParent()->getContext();
			IRBuilder<> builder(context);
			vector<Value*> args = {
				//ConstantInt::getSigned(Type::getInt32Ty(context), node->childID),
				ConstantInt::getSigned(Type::getInt32Ty(context), node->id),
				ConstantInt::getSigned(Type::getInt32Ty(context), node->type)
			};
			//if (node->type >= 0 || node->type == CALL || node->type == CALL_REC || node->type == CALL_INDIRECT) {
			if (node->type == CALL_INDIRECT || node->type == 0 || node->type == 1) {
				builder.SetInsertPoint(node->callInst);
				builder.CreateCall(entryFunc, args);
				builder.SetInsertPoint(node->callInst->getNextNode());
				builder.CreateCall(exitFunc, args);
			} else {
				// Since inner structures are instrumented later,
				// entries should be inserted at latest possible positions,
				// and exits should be inserted at earliest possible positions.
				if(node->type == FUNCTION && !node->isAttachedToAnotherNode){
					builder.SetInsertPoint(node->bb->getFirstInsertionPt());
					builder.CreateCall(entryFunc, args);
					if(node->exit){
						builder.SetInsertPoint(node->exit->getTerminator());
						builder.CreateCall(exitFunc, args);
					}
				}


			}

			for (auto child: node->children) {
				instrument(child);
			}

			node->instrumented = true;
		}

		void instrumentMain(Node* node) {
#ifdef COMBINEFUNC
			if (!node || node->removed || node->instrumented)
#else
				if (!node || node->removed || node->instrumented || node->type == COMBINE)
#endif
					return;

			// This step must be post-order!!!

			auto& context = node->bb->getParent()->getParent()->getContext();
			IRBuilder<> builder(context);
			vector<Value*> args = {
				//ConstantInt::getSigned(Type::getInt32Ty(context), node->childID),
				ConstantInt::getSigned(Type::getInt32Ty(context), node->id),
				ConstantInt::getSigned(Type::getInt32Ty(context), node->type)
			};
			//if (node->type >= 0 || node->type == CALL || node->type == CALL_REC || node->type == CALL_INDIRECT) {
			if (node->type == CALL_INDIRECT || node->type == 0 || node->type == 1) {
				builder.SetInsertPoint(node->callInst);
				builder.CreateCall(entryFunc, args);
				builder.SetInsertPoint(node->callInst->getNextNode());
				builder.CreateCall(exitFunc, args);
			} else {
				// Since inner structures are instrumented later,
				// entries should be inserted at latest possible positions,
				// and exits should be inserted at earliest possible positions.
				/*if(node->type == FUNCTION && !node->isAttachedToAnotherNode){
				  builder.SetInsertPoint(node->bb->getFirstInsertionPt());
				  builder.CreateCall(entryFunc, args);
				  if(node->exit){
				  builder.SetInsertPoint(node->exit->getTerminator());
				  builder.CreateCall(exitFunc, args);
				  }
				  }*/


			}
			for (auto child: node->children) {
				instrument(child);
			}
			node->instrumented = true;
		}


		int findOrInsertIdx(vector<StringRef>& v, StringRef& s) {
			auto idx = find(v.begin(), v.end(), s) - v.begin();
			int r = static_cast<int>(v.size());
			if (idx < r)
				r = static_cast<int>(idx);
			else
				v.push_back(s);
			return r;
		}

		void collectLocationInfo(Node* node) {
			if (!node || node->removed || node->locationInfoCollected)
				return;	
			//if(node->type != COMBINE){
			for (auto child: node->children) {
				collectLocationInfo(child);
			}
			//}

			if (node->type == FUNCTION) {
				auto inst = find_if(node->bb->begin(), node->bb->end(), [](const Instruction &instruction) {
						return !instruction.getDebugLoc().isUnknown();
						});
				if (inst != node->bb->end()) {
					auto scope = inst->getDebugLoc().getScope(node->bb->getContext());
					auto subprogram = getDISubprogram(scope);
					auto dir = subprogram.getDirectory();
					auto fn = subprogram.getFilename();
					auto line = subprogram.getLineNumber();
					node->dirID = findOrInsertIdx(dirs, dir);
					node->fileID = findOrInsertIdx(files, fn);
					node->lineNum = line;
				}

				unsigned int maxLineNum = 0;
				for(Function::iterator BB = node->func->begin(), BBE = node->func->end();BB !=BBE;++BB){
					BasicBlock& b = *BB;
					for(BasicBlock::iterator i = b.begin(),ie = b.end(); i != ie; ++i){
						MDNode* N = nullptr;
						N = i->getMetadata("dbg");
						DILocation loc(N);
						auto line = loc.getLineNumber();
						maxLineNum = (maxLineNum > line) ? maxLineNum : line;
					}

				}
				node->exitLineNum = maxLineNum;

				/*}else if (node->type == COMBINE){
				  MDNode* N = nullptr;
				  if (node->firstType == CALL)
				  N = node->callInst->getMetadata("dbg");
				  else if (node->firstType == BRANCH )//||node->firstType == LOOP)
				  N = node->bb->getTerminator()->getMetadata("dbg");
				  else if (node->firstType == LOOP){
				  BasicBlock::iterator i=nullptr;
				  for (i = node->bb->begin(); i != node->bb->end(); ++i)
				  {
				  auto branchInst = dyn_cast<BranchInst>(i);
				  if(branchInst)
				  break;
				  }
				  N = i->getMetadata("dbg");
				  }else
				  N = node->bb->begin()->getMetadata("dbg");

				  if (N) {
				  DILocation loc(N);
				  auto dir = loc.getDirectory();
				  auto fn = loc.getFilename();
				  auto line = loc.getLineNumber();
				  node->dirID = findOrInsertIdx(dirs, dir);
				  node->fileID = findOrInsertIdx(files, fn);
				  node->lineNum = line;
				  }
				  if(node->exitBb){
				  if (node->lastType == CALL)
				  N = node->callInstExit->getMetadata("dbg");
				  else if (node->lastType == BRANCH )//||node->firstType == LOOP)
				  N = node->exitBb->getTerminator()->getMetadata("dbg");
				  else if (node->lastType == LOOP){
				  N = node->bb->begin()->getMetadata("dbg");
				  BasicBlock::iterator i=nullptr;
				  for (i = node->exitBb->begin(); i != node->exitBb->end(); ++i)
				  {
				  auto branchInst = dyn_cast<BranchInst>(i);
				  if(branchInst)
				  break;
				  }
				  N = i->getMetadata("dbg");
				  }else
				  N = node->exitBb->begin()->getMetadata("dbg");

				  if (N) {
				  DILocation loc(N);
				  auto line = loc.getLineNumber();
				  node->exitLineNum = line;
				  }
				  }
				  */
		} else if(node->type != LOOP){

			MDNode* N = nullptr;
			if (node->type >= 0 || node->type == CALL || node->type == CALL_REC || node->type == CALL_INDIRECT){
				N = node->callInst->getMetadata("dbg");
				//}else if (node->type == BRANCH)
				//	N = node->bb->getTerminator()->getMetadata("dbg");
				//}else if (node->type == LOOP){
				//	N = node->bb->begin()->getMetadata("dbg");
		}else
			N = node->bb->begin()->getMetadata("dbg");
		if (N) {
			DILocation loc(N);
			auto dir = loc.getDirectory();
			auto fn = loc.getFilename();
			auto line = loc.getLineNumber();
			node->dirID = findOrInsertIdx(dirs, dir);
			node->fileID = findOrInsertIdx(files, fn);
			node->lineNum = line;

			if(node->type >= 0){  //MPI with no exitBb
				node->exitLineNum = line;
			}

		}
		if(node->exitBb && node->type != LOOP){
			if (node->type == CALL || node->type == CALL_REC || node->type == CALL_INDIRECT )
				N = node->callInst->getMetadata("dbg");
			else if (node->type == BRANCH )//||node->firstType == LOOP)
				N = node->exitBb->getTerminator()->getMetadata("dbg");
			else if (node->type == LOOP){
				N = node->exitBb->begin()->getMetadata("dbg") ;
			}else
				N = node->exitBb->begin()->getMetadata("dbg") ;

			if (N) {
				DILocation loc(N);
				auto line = loc.getLineNumber();
				if(node->type == CALL || node->type == CALL_REC || node->type == CALL_INDIRECT){
					node->exitLineNum = node->lineNum;
				}else if(node->type == LOOP){
					node->exitLineNum = line - 1;
				}else

					node->exitLineNum = line ;
			}
		}
		}

		node->locationInfoCollected = true;
		}

		void generateChildID(Node* node) {
			if (node->childIDGenerated)
				return;
			int id = 0;
			if(node->type != COMBINE){
				for (auto child: node->children) {
					if (child && !child->removed) {
						child->childID = id++;
						generateChildID(child);
					}
				}
			}
			node->numChildren = id;
			node->childIDGenerated = true;
		}

		int generateID(Node* node, int currentID) {
			if (node->idGenerated)
				return currentID;
			node->id = currentID++;

			if(node->type != COMBINE){
				for (auto child: node->children) {
					if (child && !child->removed) {
						currentID = generateID(child, currentID);
					}
				}
			}
			node->idGenerated = true;
#ifdef version36
			maxNodeId = (maxNodeId >= currentID ) ? maxNodeId : currentID ;
#endif
			return currentID;
		}
		void compNodeExpand(Node* node) {
			if (!node || node->compNodeExpanded)
				return;
			//if (node-> children.size())
			bool hasChildren = false, hasUncombinedChild = false;
			bool firstRemovedChildFlag = true;
			BasicBlock* pre = nullptr;
			BasicBlock* exi = nullptr;
			BasicBlock* exiBb = nullptr;
			BasicBlock* b = nullptr;
			CallInst* callInstPre = nullptr;
			CallInst* callInstExi = nullptr;
			int firType = -10, lasType = -10;

			int curChild = 0;

			for (auto child: node->children) {
				if(!child || child == nullptr || (void*)-1 == child ){
					continue;
				}
				//errs()<<child->id<<"\n";
				if(child->removed) {
					//errs()<<"line:"<<__LINE__<<"\n";
					hasUncombinedChild = true;
					if (firstRemovedChildFlag) {
						firType = child->type;
						lasType = child->type;
						firstRemovedChildFlag = false;
						//errs()<<"line:"<<__LINE__<<"type:"<<child->type<<"\n";
						if(child->type == LOOP){
							pre = child->preheader;
							exi = child->exit;
							b = child->bb;
						}else if(child->type == BRANCH || child->type == FUNCTION){
							pre = child->bb;
							exi = child->exit;
							b = child->bb;
						}else if(child->type == COMPOUND){
							pre = child->bb;
							b = child->bb;
						}else if(child->type == CALL){
							b = child->bb;
							callInstPre = child->callInst;
							callInstExi = child->callInst;
						}
					}else {
						lasType = child->type;
						if(child->type == LOOP || child->type == BRANCH || child->type == FUNCTION ){
							exi = child->exit;
							exiBb = child->bb;
						}else if(child->type == CALL){
							callInstExi = child->callInst;
							exiBb = child->bb;
						}else{
							exiBb = child->bb;
						}
					}
				}else if(!child->removed) {  //create new COMBINE before this child
					//errs()<<"line:"<<__LINE__<<"\n";
					if(hasUncombinedChild){
						//errs()<<"line:"<<__LINE__<<"\n";
						//Node * cnode = new Node(COMBINE, b);
						Node* cnode = node->children[curChild-1];
						cnode->type = COMBINE;
						cnode->bb = b;
						cnode->firstType = firType;
						cnode->lastType = lasType;
						cnode->preheader = pre;
						cnode->exit = exi;
						cnode->exitBb = exiBb;
						cnode->callInst = callInstPre;
						cnode->callInstPreheader = callInstPre;
						cnode->callInstExit = callInstExi;
						//errs()<<"line:"<<__LINE__<<"\n";
						cnode->expanded = true;
						cnode->trimmed = true;
						cnode->removed = false;
						//errs()<<"line:"<<__LINE__<<"\n";
						pre = nullptr;
						exi = nullptr;
						b = nullptr;
						callInstPre = nullptr;
						callInstExi = nullptr;
						firType = -10;
						lasType = -10;
						//errs()<<"line:"<<__LINE__<<"\n";
						hasChildren = true;
						hasUncombinedChild = false;
						firstRemovedChildFlag = true;
						//errs()<<"line:"<<__LINE__<<"\n";
						//node->children.insert(node->children.begin()+curChild,cnode);
						//errs()<<"line:"<<__LINE__<<"\n";
					}
					//errs()<<"line:"<<__LINE__<<"\n";
					compNodeExpand(child);
					//errs()<<"line:"<<__LINE__<<"\n";
				}
				//errs()<<"line:"<<__LINE__<<"\n";
				curChild++;
			}
			//errs()<<"line:"<<__LINE__<<"\n";
			if (hasChildren && hasUncombinedChild){ // COMPNODE after the last child
				//errs()<<"line:"<<__LINE__<<"\n";
				//Node * cnode = new Node(COMBINE, b);
				Node * cnode = node->children[node->children.size()-1];
				cnode->type = COMBINE;
				cnode->bb = b ;
				cnode->firstType = firType;
				cnode->lastType = lasType;
				cnode->preheader = pre;
				cnode->exit = exi;
				cnode->callInst = callInstPre;
				cnode->callInstPreheader = callInstPre;
				cnode->callInstExit = callInstExi;

				cnode->expanded = true;
				cnode->trimmed = true;
				cnode->removed = false;
				pre = nullptr;
				exi = nullptr;
				b = nullptr;
				callInstPre = nullptr;
				callInstExi = nullptr;
				firType = -10;
				lasType = -10;

				//node->children.insert(node->children.end(),cnode);
			}
			node->compNodeExpanded = true;
		}


		bool trim(Node* node) {
			// Return true if this node should not be removed
			if (!node)
				return false;
			if (node->trimmed)
				return !(node->removed);

			if (node->type >= 0 || node->type == CALL_REC || node->type == CALL_INDIRECT) {
				node->removed = false;
				node->trimmed = true;
				return true;
			}

			bool reserved = false;
			for (auto child: node->children) {
				reserved |= trim(child);
			}

			node->removed = !reserved;
			node->trimmed = true;
			return reserved;
		}

		void expand(Node* node) {
			if (!node || node->expanded)
				return;

			for (auto child: node->children) {
				expand(child);
			}

			if (node->type == CALL) {
				auto callee = node->func;
				auto calleeNode = func2node[callee];
				assert(calleeNode);
				expand(calleeNode);
				node->children.push_back(calleeNode);
#ifdef version36
				calleeNode->isAttachedToAnotherNode = true;
#endif
			}

			node->expanded = true;
		}

		NodeType getRegionType(Region* region, LoopInfo* loopInfo) {
			// Infer region type (loop/branch/function)
			auto bb = region->getEntry();
			auto loop = loopInfo->getLoopFor(bb);
			if (region->isTopLevelRegion()) {
				// Function region
				return FUNCTION;
			} else if (loop && region->contains(loop) && loop->getHeader() == bb) {
				// The entry of the region is a loop header,
				// which indicates that this region represents a loop
				return LOOP;
			} else if (bb->getTerminator()->getNumSuccessors() > 1) {
				// A branch
				return BRANCH;
			} else {
				assert("Cannot decide region type" && false);
			}
		}

		void handleBasicBlock(BasicBlock* bb, Node* parent) {
			for (auto iter = bb->begin(); iter != bb->end(); ++iter) {
				auto callInst = dyn_cast<CallInst>(iter);
				if (callInst) {
					auto callee = callInst->getCalledFunction();

					// Try to find the indirectly called function
					// Also for fortran MPI calls, which is represented as a bitcasted form by dragonegg
					if (!callee)
						callee = dyn_cast<Function>(callInst->getCalledValue()->stripPointerCasts());

					if (callee) {
						string funcName = callee->getName();
						auto result = instFuncMap.find(funcName);
						if (result != instFuncMap.end()) {
							// If it is a MPI function, do instrumentation
							parent->children.push_back(new Node(result->second, bb, nullptr, callInst));
						} else if (!callee->isDeclaration()) {
							auto rec = recSet.find(callee);
							if (rec == recSet.end()) {
								// Found a non-recursive function call, add it to funcQue
								auto callNode = new Node(CALL, bb, callee, callInst);
								callNode -> exitBb = bb;
								parent->children.push_back(callNode);
								if (func2node.find(callee) == func2node.end()) {
									funcQue.push(callee);
									func2node[callee] = nullptr; // Placeholder
								}
							} else {
								// Found a recursive function call
								auto recNode = new Node(CALL_REC, bb, callee, callInst);
								recNode -> exitBb = bb;
								parent->children.push_back(recNode);
							}
						}
					} else {
						// Indirect call
						auto callNode = new Node(CALL_INDIRECT, bb, callee, callInst);
						callNode -> exitBb = bb;
						parent->children.push_back(callNode);
					}
				}
			}
		}

		void traverse(BasicBlock* start, Region* region, LoopInfo* loopInfo, Node* parent) {
			unordered_set<BasicBlock*> visited;
			queue<BasicBlock*> q;
			auto now = start;
			q.push(now);
			visited.insert(now);
			while (!q.empty()) {
				now = q.front();
				q.pop();

				auto subRegion = region->getSubRegionNode(now);
				if (subRegion) {
					handleRegion(subRegion, loopInfo, parent);
					auto subExit = subRegion->getExit();
					if (region->contains(subExit) && visited.find(subExit) == visited.end()) {
						q.push(subExit);
						visited.insert(subExit);
					}
				} else {
					assert(region->getRegionInfo()->getRegionFor(now) == region);

					handleBasicBlock(now, parent);

					auto termInst = now->getTerminator();
					auto numSuccs = termInst->getNumSuccessors();
					for (auto i = 0u; i < numSuccs; ++i) {
						auto succ = termInst->getSuccessor(i);
						if (region->contains(succ) && visited.find(succ) == visited.end()) {
							q.push(succ);
							visited.insert(succ);
						}
					}
				}
			}
		}

		bool checkBranchIntersection(BasicBlock* brBlock, Region* region) {
			unordered_map<BasicBlock*, unsigned int> bb2branch;
			queue<BasicBlock*> q;
			BasicBlock* now;
			auto termInst = brBlock->getTerminator();
			auto numSuccs = termInst->getNumSuccessors();
			for (auto i = 0u; i < numSuccs; ++i) {
				auto cbb = termInst->getSuccessor(i);
				if (region->contains(cbb)) {
					q.push(cbb);
					while (!q.empty()) {
						now = q.front();
						q.pop();

						auto result = bb2branch.find(now);
						if (result != bb2branch.end()) {
							if (result->second != i) {
								// Intersection
								return true;
							}
						} else {
							bb2branch.insert({now, i});

							auto subRegion = region->getSubRegionNode(now);
							// Do not go into subregions
							if (!subRegion) {
								auto ti = now->getTerminator();
								auto ns = ti->getNumSuccessors();
								for (auto j = 0u; j < ns; ++j) {
									auto succ = ti->getSuccessor(j);
									if (region->contains(succ)) {
										q.push(succ);
									}
								}
							} else {
								auto subExit = subRegion->getExit();
								if (region->contains(subExit)) {
									q.push(subExit);
								}
							}
						}
					}
				}
			}
			return false;
		}

		void handleRegion(Region* region, LoopInfo* loopInfo, Node* parent) {
			auto bb = region->getEntry();
			auto type = getRegionType(region, loopInfo);

			if (type == BRANCH) {
				auto subRegion = region->getSubRegionNode(region->getEntry());
				assert(!subRegion);

				// Check if branches have intersection
				if (checkBranchIntersection(bb, region)) {
					traverse(bb, region, loopInfo, parent);
					return;
				}

				handleBasicBlock(bb, parent);
				/*Node* node = new Node(BRANCH, bb);
				  node->exit = region->getExit();
				  assert(node->exit);
				  parent->children.push_back(node);*/

				auto termInst = bb->getTerminator();
				auto numSuccs = termInst->getNumSuccessors(); // Could be > 2 for switch stmt
				for (auto i = 0u; i < numSuccs; ++i) {
					auto cbb = termInst->getSuccessor(i);
					if (region->contains(cbb)) {
						/*Node* cnode = new Node(COMPOUND, cbb);
						  node->children.push_back(cnode);*/
						traverse(cbb, region, loopInfo, parent);
					} else {
						// Empty branch
						assert(cbb == region->getExit());
						//node->children.push_back(nullptr);
					}
				}
				return;
			}

			Node* node = nullptr;
			if (type == LOOP) {
				bb = *(region->block_begin());
				node = new Node(type, bb);
				auto loop = loopInfo->getLoopFor(bb);
				node->preheader = loop->getLoopPreheader();
				assert(node->preheader);
				assert(node->preheader->getTerminator()->getNumSuccessors() == 1);
				node->latch = loop->getLoopLatch();
				assert(node->latch);
				node->exit = region->getExit();
				//node->exitBb = region->getExitingBlock();
				unsigned int maxLineNum = 0;
				//unsigned int minLineNum = 123123123;
				for(Region::block_iterator BB = region->block_begin(), BBE = region->block_end();BB !=BBE;++BB){
					BasicBlock* b = *BB;
					for(BasicBlock::iterator i = b->begin(),ie = b->end(); i != ie; ++i){
						MDNode* N = nullptr;
						N = i->getMetadata("dbg");
						if(N){
							DILocation loc(N);
							auto dir = loc.getDirectory();
							auto fn = loc.getFilename();
							auto line = loc.getLineNumber();
							node->dirID = findOrInsertIdx(dirs, dir);
							node->fileID = findOrInsertIdx(files, fn);
							maxLineNum = (maxLineNum > line) ? maxLineNum : line;
							//minLineNum = (minLineNum < line) ? minLineNum : line;
						}
					}

				}
				//node->lineNum = loopInfo.getStartLoc().getLine();
				//node->lineNum = minLineNum;
				node->exitLineNum = maxLineNum;
				MDNode* N = nullptr;
				BasicBlock::iterator i=nullptr;
				auto termInst = node->preheader->getTerminator();
				auto numSuccs = termInst->getNumSuccessors();
				if(numSuccs == 1){
					auto cbb = termInst->getSuccessor(0);
					N = cbb -> getTerminator() -> getMetadata("dbg");

				}else{
					errs() << *i <<"\n";
					N = node->preheader-> getTerminator()->getMetadata("dbg");
				}
				//N = node->preheader->end()->getMetadata("dbg");
				if (N) {
					DILocation loc(N);
					auto dir = loc.getDirectory();
					auto fn = loc.getFilename();
					auto line = loc.getLineNumber();
					node->dirID = findOrInsertIdx(dirs, dir);
					node->fileID = findOrInsertIdx(files, fn);
					node->lineNum = line;
				}
				assert(node->exit);
			} else if (type == FUNCTION) {
				// Precondition: mergereturn pass runned (Only 1 exit for a function)
				auto func = region->getEntry()->getParent();
				node = new Node(type, bb, func);
				for (auto& bi: *func) {
					auto termInst = bi.getTerminator();
					if (isa<ReturnInst>(termInst)) {
						node->exit = termInst->getParent();
						node->exitBb = region->getEntry();
						break;
					}
				}
				//node->exitBb = bi;
				//if(!node->exit){
				//	node->exit == region->getExit();
				//}
			} else {
				assert("Unexpected region type" && false);
			}
			parent->children.push_back(node);
			traverse(bb, region, loopInfo, node);
		}

		Node* buildTree(Function *f) {
			auto loopInfo = &getAnalysis<LoopInfo>(*f);
			auto regionInfo = &getAnalysis<RegionInfo>(*f);

			auto root = new Node(FUNCTION, &f->getEntryBlock());
			//root -> exitBb = &f->getExitBlock();
			handleRegion(regionInfo->getTopLevelRegion(), loopInfo, root);
			assert(root->children.size() == 1);
			auto funcRoot = root->children[0];
			delete root;

			return funcRoot;
		}
		};

	}

	char IRSPass::ID = 0;
	static RegisterPass<IRSPass> X("irs", "IRS Pass");
