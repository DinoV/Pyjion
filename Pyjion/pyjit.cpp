#include "pyjit.h"

#include <corjit.h>
#include <utilcode.h>
#include <openum.h>
#include <frameobject.h>
#include <opcode.h>

#include "cee.h"
#include "jitinfo.h"
#include "codemodel.h"
#include "ilgen.h"
#include "intrins.h"

//#define DEBUG_TRACE

#define LD_FIELDA(type, field) m_il.ld_i(offsetof(type, field)); m_il.add(); 
#define LD_FIELD(type, field) m_il.ld_i(offsetof(type, field)); m_il.add(); m_il.ld_ind_i();
#define ST_FIELD(type, field) m_il.ld_i(offsetof(type, field)); m_il.add(); m_il.st_ind_i();

#define BLOCK_CONTINUES 0x01
#define BLOCK_RETURNS	0x02
#define BLOCK_BREAKS	0x04

#define NEXTARG() *(unsigned short*)&m_byteCode[i + 1]; i+= 2

extern CorJitInfo g_corJitInfo;
extern ICorJitCompiler* g_jit;

struct ExceptionVars {
	Local PrevExc, PrevExcVal, PrevTraceback;

	ExceptionVars() {
	}

	ExceptionVars(Local prevExc, Local prevExcVal, Local prevTraceback) {
		PrevExc = prevExc;
		PrevExcVal = prevExcVal;
		PrevTraceback = prevTraceback;
	}
};

struct EhInfo {
	bool IsFinally;
	int Flags;

	EhInfo(bool isFinally) {
		IsFinally = isFinally;
		Flags = 0;
	}
};

struct BlockInfo {
	Label Raise,		// our raise stub label, prepares the exception
		ReRaise,		// our re-raise stub label, prepares the exception w/o traceback update
		ErrorTarget;	// the actual label for the handler
	int EndOffset, Kind, Flags, ContinueOffset;
	ExceptionVars ExVars;

	BlockInfo() {
	}

	BlockInfo(Label raise, Label reraise, Label errorTarget, int endOffset, int kind, int flags = 0, int continueOffset = 0) {
		Raise = raise;
		ReRaise = reraise;
		ErrorTarget = errorTarget;
		EndOffset = endOffset;
		Kind = kind;
		Flags = flags;
		ContinueOffset = continueOffset;
	}
};


Module *g_module;
ICorJitCompiler* g_jit;

class Jitter {
	PyCodeObject *m_code;
	// pre-calculate some information...
	ILGenerator m_il;
	// Stores information for a stack allocated local used for sequence unpacking.  We need to allocate
	// one of these when we enter the method, and we use it if we don't have a sequence we can efficiently
	// unpack.
	unordered_map<int, Local> m_sequenceLocals;
	unsigned char *m_byteCode;
	size_t m_size;
	// m_blockStack is like Python's f_blockstack which lives on the frame object, except we only maintain
	// it at compile time.  Blocks are pushed onto the stack when we enter a loop, the start of a try block,
	// or into a finally or exception handler.  Blocks are popped as we leave those protected regions.
	// When we pop a block associated with a try body we transform it into the correct block for the handler
	vector<BlockInfo> m_blockStack;		
	// All of the exception handlers defined in the method.  After generating the method we'll generate helper
	// targets which dispatch to each of the handlers.
	vector<BlockInfo> m_allHandlers;
	// Tracks the state for the handler block, used for END_FINALLY processing.  We push these with a SETUP_EXCEPT/
	// SETUP_FINALLY, update them when we hit the POP_EXCEPT so we have information about the try body, and then
	// finally pop them when we hit the SETUP_FINALLY.  These are independent from the block stack because they only
	// contain information about exceptions, and don't change as we transition from the body of the try to the body
	// of the handler.
	vector<EhInfo> m_ehInfo;
	// Labes that map from a Python byte code offset to an ilgen label.  This allows us to branch to any
	// byte code offset.
	unordered_map<int, Label> m_offsetLabels;

public:
	Jitter(PyCodeObject *code) : m_il(g_module, CORINFO_TYPE_NATIVEINT, std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT) }){
		this->m_code = code;
		this->m_byteCode = (unsigned char *)((PyBytesObject*)code->co_code)->ob_sval;
		this->m_size = PyBytes_Size(code->co_code);
	}

	PVOID Compile() {
		PreProcess();
		return CompileWorker();
	}

private:
	bool can_skip_lasti_update(int opcode) {
		switch (opcode) {
		case DUP_TOP:
		case SETUP_EXCEPT:
		case NOP:
		case ROT_TWO:
		case ROT_THREE:
		case POP_BLOCK:
		case POP_JUMP_IF_FALSE:
		case POP_JUMP_IF_TRUE:
		case POP_TOP:
		case DUP_TOP_TWO:
		case BREAK_LOOP:
		case CONTINUE_LOOP:
		case END_FINALLY:
		case LOAD_CONST:
		case JUMP_FORWARD:
			return true;
		}
		return false;
	}

	void load_frame() {
		m_il.ld_arg(0);
	}

	void load_local(int oparg) {
		load_frame();
		m_il.ld_i(offsetof(PyFrameObject, f_localsplus) + oparg * sizeof(size_t));
		m_il.add();
		m_il.ld_ind_i();
	}

	void incref() {
		LD_FIELDA(PyObject, ob_refcnt);
		m_il.dup();
		m_il.ld_ind_i4();
		m_il.ld_i4(1);
		m_il.add();
		m_il.st_ind_i4();
	}

	void decref() {
		m_il.emit_call(METHOD_DECREF_TOKEN);
		//LD_FIELDA(PyObject, ob_refcnt);
		//m_il.push_back(CEE_DUP);
		//m_il.push_back(CEE_LDIND_I4);
		//m_il.push_back(CEE_LDC_I4_1);
		//m_il.push_back(CEE_SUB);
		////m_il.push_back(CEE_DUP);
		//// _Py_Dealloc(_py_decref_tmp)

		//m_il.push_back(CEE_STIND_I4);
	}

	void build_tuple(int argCnt) {
		auto valueTmp = m_il.define_local(Parameter(CORINFO_TYPE_NATIVEINT));
		auto tupleTmp = m_il.define_local(Parameter(CORINFO_TYPE_NATIVEINT));

		m_il.push_ptr((void*)argCnt);
		m_il.emit_call(METHOD_PYTUPLE_NEW);
		check_error(-1, "new tuple failed");
		m_il.st_loc(tupleTmp);

		for (int arg = argCnt - 1; arg >= 0; arg--) {
			// save the argument into a temporary...
			m_il.st_loc(valueTmp);

			// load the address of the tuple item...
			m_il.ld_loc(tupleTmp);
			m_il.ld_i(arg * sizeof(size_t) + offsetof(PyTupleObject, ob_item));
			m_il.add();

			// reload the value
			m_il.ld_loc(valueTmp);

			// store into the array
			m_il.st_ind_i();
		}
		m_il.ld_loc(tupleTmp);

		m_il.free_local(valueTmp);
		m_il.free_local(tupleTmp);
	}

	void build_list(int argCnt) {

		m_il.push_ptr((void*)argCnt);
		m_il.emit_call(METHOD_PYLIST_NEW);

		if (argCnt != 0) {
			auto valueTmp = m_il.define_local(Parameter(CORINFO_TYPE_NATIVEINT));
			auto listTmp = m_il.define_local(Parameter(CORINFO_TYPE_NATIVEINT));
			auto listItems = m_il.define_local(Parameter(CORINFO_TYPE_NATIVEINT));

			m_il.dup();
			m_il.st_loc(listTmp);

			// load the address of the tuple item...
			m_il.ld_i(offsetof(PyListObject, ob_item));
			m_il.add();
			m_il.ld_ind_i();

			m_il.st_loc(listItems);

			for (int arg = argCnt - 1; arg >= 0; arg--) {
				// save the argument into a temporary...
				m_il.st_loc(valueTmp);

				// load the address of the tuple item...
				m_il.ld_loc(listItems);
				m_il.ld_i(arg * sizeof(size_t));
				m_il.add();

				// reload the value
				m_il.ld_loc(valueTmp);

				// store into the array
				m_il.st_ind_i();
			}

			// update the size of the list...
			m_il.ld_loc(listTmp);
			m_il.dup();
			m_il.ld_i(offsetof(PyVarObject, ob_size));
			m_il.add();
			m_il.push_ptr((void*)argCnt);
			m_il.st_ind_i();

			m_il.free_local(valueTmp);
			m_il.free_local(listTmp);
			m_il.free_local(listItems);
		}

	}

	void build_set(int argCnt) {
		m_il.load_null();
		m_il.emit_call(METHOD_PYSET_NEW);

		if (argCnt != 0) {
			auto valueTmp = m_il.define_local(Parameter(CORINFO_TYPE_NATIVEINT));
			auto setTmp = m_il.define_local(Parameter(CORINFO_TYPE_NATIVEINT));

			m_il.st_loc(setTmp);

			for (int arg = argCnt - 1; arg >= 0; arg--) {
				// save the argument into a temporary...
				m_il.st_loc(valueTmp);

				// load the address of the tuple item...
				m_il.ld_loc(setTmp);
				m_il.ld_loc(valueTmp);
				m_il.emit_call(METHOD_PYSET_ADD);
				m_il.pop();
			}

			m_il.ld_loc(setTmp);
			m_il.free_local(valueTmp);
			m_il.free_local(setTmp);
		}
	}

	void build_map(int argCnt) {
		m_il.push_ptr((void*)argCnt);
		m_il.emit_call(METHOD_PYDICT_NEWPRESIZED);
		check_error(-1, "new dict failed");
		{
			// 3.6 doesn't have STORE_OP and instead does it all in BUILD_MAP...
			if (argCnt > 0) {
				auto map = m_il.define_local(Parameter(CORINFO_TYPE_NATIVEINT));
				m_il.st_loc(map);
				for (int curArg = 0; curArg < argCnt; curArg++) {
					m_il.ld_loc(map);
					m_il.emit_call(METHOD_STOREMAP_TOKEN);
					check_int_error(-1);
				}
				m_il.ld_loc(map);

				m_il.free_local(map);
			}
		}
	}

	Label getOffsetLabel(int jumpTo) {
		auto jumpToLabelIter = m_offsetLabels.find(jumpTo);
		Label jumpToLabel;
		if (jumpToLabelIter == m_offsetLabels.end()) {
			m_offsetLabels[jumpTo] = jumpToLabel = m_il.define_label();
		}
		else{
			jumpToLabel = jumpToLabelIter->second;
		}
		return jumpToLabel;
	}

	// Checks to see if we have a null value as the last value on our stack
	// indicating an error, and if so, branches to our current error handler.
	void check_error(int curIndex, const char* reason) {
		auto noErr = m_il.define_label();
		m_il.dup();
		m_il.load_null();
		m_il.branch(BranchNotEqual, noErr);
		// we need to issue a leave to clear the stack as we may have
		// values on the stack...
#ifdef DEBUG_TRACE
		char* tmp = (char*)malloc(100);
		sprintf_s(tmp, 100, "Error at index %d %s %s", curIndex, PyUnicode_AsUTF8(m_code->co_name), reason);
		m_il.push_ptr(tmp);
		m_il.emit_call(METHOD_DEBUG_TRACE);
#endif

		m_il.branch(BranchLeave, GetEHBlock().Raise);	
		m_il.mark_label(noErr);
	}

	void check_int_error_leave(int curIndex) {
		auto noErr = m_il.define_label();
		m_il.ld_i4(0);
		m_il.branch(BranchEqual, noErr);
		// we need to issue a leave to clear the stack as we may have
		// values on the stack...
#ifdef DEBUG_TRACE
		char* tmp = (char*)malloc(100);
		sprintf_s(tmp, 100, "Int Error at index %d %s", curIndex, PyUnicode_AsUTF8(m_code->co_name));
		m_il.push_ptr(tmp);
		m_il.emit_call(METHOD_DEBUG_TRACE);
#endif

		m_il.branch(BranchLeave, GetEHBlock().Raise);
		m_il.mark_label(noErr);
	}

	// Checks to see if we have a non-zero error code on the stack, and if so,
	// branches to the current error handler.  Consumes the error code in the process
	void check_int_error(int curIndex) {
		check_int_error_leave(curIndex);
		//m_il.ld_i4(0);
		//m_il.branch(BranchNotEqual, GetEHBlock().Raise);
	}

	void unwind_eh(ExceptionVars& exVars) {
		m_il.ld_loc(exVars.PrevExc);
		m_il.ld_loc(exVars.PrevExcVal);
		m_il.ld_loc(exVars.PrevTraceback);
		m_il.emit_call(METHOD_UNWIND_EH);
	}

	BlockInfo GetEHBlock() {
		for (size_t i = m_blockStack.size() - 1; i != -1; i--) {
			if (m_blockStack[i].Kind != SETUP_LOOP) {
				return m_blockStack[i];
			}
		}
		assert(FALSE);
		return BlockInfo();
	}

	void mark_offset_label(int index) {
		auto existingLabel = m_offsetLabels.find(index);
		if (existingLabel != m_offsetLabels.end()) {
			m_il.mark_label(existingLabel->second);
		}
		else{
			auto label = m_il.define_label();
			m_offsetLabels[index] = label;
			m_il.mark_label(label);
		}

	}

	void PreProcess() {
		int oparg;
		for (int i = 0; i < m_size; i++) {
			auto byte = m_byteCode[i];
			if (HAS_ARG(byte)){
				oparg = NEXTARG();
			}

			switch (byte) {
			case UNPACK_EX:
			{
				auto sequenceTmp = m_il.define_local(Parameter(CORINFO_TYPE_NATIVEINT));
				m_il.ld_i(((oparg & 0xFF) + (oparg >> 8)) * sizeof(void*));
				m_il.localloc();
				m_il.st_loc(sequenceTmp);

				m_sequenceLocals[i] = sequenceTmp;
			}
				break;
			case UNPACK_SEQUENCE:
				// we need a buffer for the slow case, but we need 
				// to avoid allocating it in loops.
				auto sequenceTmp = m_il.define_local(Parameter(CORINFO_TYPE_NATIVEINT));
				m_il.ld_i(oparg * sizeof(void*));
				m_il.localloc();
				m_il.st_loc(sequenceTmp);

				m_sequenceLocals[i] = sequenceTmp;
				break;
			}
		}
	}

	PVOID CompileWorker() {
		int oparg;
		Label ok;

#ifdef DEBUG_TRACE
		m_il.push_ptr(PyUnicode_AsUTF8(m_code->co_name));
		m_il.emit_call(METHOD_DEBUG_TRACE);

		load_frame();
		m_il.emit_call(METHOD_DEBUG_DUMP_FRAME);
#endif

		auto raiseLabel = m_il.define_label();
		auto reraiseLabel = m_il.define_label();

		auto lasti = m_il.define_local(Parameter(CORINFO_TYPE_NATIVEINT));
		load_frame();
		m_il.ld_i(offsetof(PyFrameObject, f_lasti));
		m_il.add();
		m_il.st_loc(lasti);

		load_frame();
		m_il.emit_call(METHOD_PY_PUSHFRAME);

		m_blockStack.push_back(BlockInfo(raiseLabel, reraiseLabel, Label(), -1, NOP));

		auto tb = m_il.define_local(Parameter(CORINFO_TYPE_NATIVEINT));
		auto ehVal = m_il.define_local(Parameter(CORINFO_TYPE_NATIVEINT));
		auto excType = m_il.define_local(Parameter(CORINFO_TYPE_NATIVEINT));
		auto retValue = m_il.define_local(Parameter(CORINFO_TYPE_NATIVEINT));
		auto retLabel = m_il.define_label();

		for (int i = 0; i < m_size; i++) {
#ifdef DEBUG_TRACE
			//char * tmp = (char*)malloc(8);
			//sprintf_s(tmp, 8, "%d", i);
			//m_il.push_ptr(tmp);
			//m_il.emit_call(METHOD_DEBUG_TRACE);
#endif

			auto byte = m_byteCode[i];

			// See FOR_ITER for special handling of the offset label
			if (byte != FOR_ITER) {
				mark_offset_label(i);
			}

			// update f_lasti
			if (!can_skip_lasti_update(m_byteCode[i])) {
				m_il.ld_loc(lasti);
				m_il.ld_i(i);
				m_il.st_ind_i4();
			}

			if (HAS_ARG(byte)){
				oparg = NEXTARG();
			}
		processOpCode:
			switch (byte) {
			case NOP:
				break;
			case ROT_TWO:
			{
				auto top = m_il.define_local(Parameter(CORINFO_TYPE_NATIVEINT));
				auto second = m_il.define_local(Parameter(CORINFO_TYPE_NATIVEINT));

				m_il.st_loc(top);
				m_il.st_loc(second);

				m_il.ld_loc(top);
				m_il.ld_loc(second);

				m_il.free_local(top);
				m_il.free_local(second);
			}
				break;
			case ROT_THREE:
			{
				auto top = m_il.define_local(Parameter(CORINFO_TYPE_NATIVEINT));
				auto second = m_il.define_local(Parameter(CORINFO_TYPE_NATIVEINT));
				auto third = m_il.define_local(Parameter(CORINFO_TYPE_NATIVEINT));

				m_il.st_loc(top);
				m_il.st_loc(second);
				m_il.st_loc(third);

				m_il.ld_loc(top);
				m_il.ld_loc(third);
				m_il.ld_loc(second);

				m_il.free_local(top);
				m_il.free_local(second);
				m_il.free_local(third);
			}
				break;
			case POP_TOP:
				decref();
				break;
			case DUP_TOP:
				m_il.dup();
				m_il.dup();
				incref();
				break;
			case DUP_TOP_TWO:
			{
				auto top = m_il.define_local(Parameter(CORINFO_TYPE_NATIVEINT));
				auto second = m_il.define_local(Parameter(CORINFO_TYPE_NATIVEINT));

				m_il.st_loc(top);
				m_il.st_loc(second);

				m_il.ld_loc(second);
				m_il.ld_loc(top);
				m_il.ld_loc(second);
				m_il.ld_loc(top);

				m_il.ld_loc(top);
				incref();
				m_il.ld_loc(second);
				incref();

				m_il.free_local(top);
				m_il.free_local(second);

			}
				break;
			case COMPARE_OP:
			{
				auto compareType = oparg;
				switch (compareType) {
				case PyCmp_IS:
				case PyCmp_IS_NOT:
					// TODO: Missing dec refs here...
				{
					Label same = m_il.define_label();
					Label done = m_il.define_label();
					m_il.branch(BranchEqual, same);
					m_il.push_ptr(compareType == PyCmp_IS ? Py_False : Py_True);
					m_il.branch(BranchAlways, done);
					m_il.mark_label(same);
					m_il.push_ptr(compareType == PyCmp_IS ? Py_True : Py_False);
					m_il.mark_label(done);
					m_il.dup();
					incref();
				}
					break;
				case PyCmp_IN:
					m_il.emit_call(METHOD_CONTAINS_TOKEN);
					check_error(i, "contains");
					break;
				case PyCmp_NOT_IN:
					m_il.emit_call(METHOD_NOTCONTAINS_TOKEN);
					check_error(i, "not contains");
					break;
				case PyCmp_EXC_MATCH:
					m_il.emit_call(METHOD_COMPARE_EXCEPTIONS);
					check_error(i, "compare ex");
					break;
				default:
					m_il.ld_i(oparg);
					m_il.emit_call(METHOD_RICHCMP_TOKEN);
					check_error(i, "rich cmp");
					break;
				}
			}
				break;
			case SETUP_LOOP:
				// offset is relative to end of current instruction
				m_blockStack.push_back(
					BlockInfo(
					m_blockStack.back().Raise,
					m_blockStack.back().ReRaise,
					m_blockStack.back().ErrorTarget,
					oparg + i + 1,
					SETUP_LOOP
					)
					);
				break;
			case BREAK_LOOP:
			case CONTINUE_LOOP:
				// if we have finally blocks we need to unwind through them...
				// used in exceptional case...
			{
				bool inFinally = false;
				size_t loopIndex = -1, clearEh = -1;
				for (size_t i = m_blockStack.size() - 1; i != -1; i--) {
					if (m_blockStack[i].Kind == SETUP_LOOP) {
						// we found our loop, we don't need additional processing...
						loopIndex = i;
						break;
					}
					else if (m_blockStack[i].Kind == END_FINALLY || m_blockStack[i].Kind == POP_EXCEPT) {
						if (clearEh == -1) {
							clearEh = i;
						}
					} else if (m_blockStack[i].Kind == SETUP_FINALLY) {
						// we need to run the finally before continuing to the loop...
						// That means we need to spill the stack, branch to the finally,
						// run it, and have the finally branch back to our oparg.
						// CPython handles this by pushing the opcode to continue at onto
						// the stack, and then pushing an integer value which indicates END_FINALLY
						// should continue execution.  Our END_FINALLY expects only a single value
						// on the stack, and we also need to preserve any loop variables.
						m_blockStack.data()[i].Flags |= byte == BREAK_LOOP ? BLOCK_BREAKS : BLOCK_CONTINUES;

						if (!inFinally) {
							// only emit the branch to the first finally, subsequent branches
							// to other finallys will be handled by the END_FINALLY code.  But we
							// need to mark those finallys as needing special handling.
							inFinally = true;
							if (clearEh != -1) {
								unwind_eh(m_blockStack[clearEh].ExVars);
							}
							m_il.ld_i4(byte == BREAK_LOOP ? BLOCK_BREAKS : BLOCK_CONTINUES);
							m_il.branch(BranchAlways, m_blockStack[i].ErrorTarget);
							if (byte == CONTINUE_LOOP) {
								m_blockStack.data()[i].ContinueOffset = oparg;
							}
						}
					}
				}

				if (!inFinally) {
					if (clearEh != -1) {
						unwind_eh(m_blockStack[clearEh].ExVars);
					}
					if (byte == BREAK_LOOP) {
						assert(loopIndex != -1);
						m_il.branch(BranchAlways, getOffsetLabel(m_blockStack[loopIndex].EndOffset));
					}
					else{
						m_il.branch(BranchAlways, getOffsetLabel(oparg));
					}
				}

			}
				break;
			case LOAD_BUILD_CLASS:
				load_frame();
				m_il.emit_call(METHOD_GETBUILDCLASS_TOKEN);
				check_error(i, "get build class");
				break;
			case JUMP_ABSOLUTE:
			{
				m_il.branch(BranchAlways, getOffsetLabel(oparg));
				break;
			}
			case JUMP_FORWARD:
				m_il.branch(BranchAlways, getOffsetLabel(oparg + i + 1));
				break;
			case JUMP_IF_FALSE_OR_POP:
			case JUMP_IF_TRUE_OR_POP:
			{
				auto tmp = m_il.define_local(Parameter(CORINFO_TYPE_NATIVEINT));
				m_il.st_loc(tmp);


				auto noJump = m_il.define_label();
				auto willJump = m_il.define_label();
				// fast checks for true/false...
				m_il.ld_loc(tmp);
				m_il.push_ptr(byte == JUMP_IF_FALSE_OR_POP ? Py_True : Py_False);
				m_il.compare_eq();
				m_il.branch(BranchTrue, noJump);

				m_il.ld_loc(tmp);
				m_il.push_ptr(byte == JUMP_IF_FALSE_OR_POP ? Py_False : Py_True);
				m_il.compare_eq();
				m_il.branch(BranchTrue, willJump);

				// Use PyObject_IsTrue
				m_il.ld_loc(tmp);
				m_il.emit_call(METHOD_PYOBJECT_ISTRUE);
				m_il.ld_i(0);
				m_il.compare_eq();
				m_il.branch(byte == JUMP_IF_FALSE_OR_POP ? BranchFalse : BranchTrue, noJump);

				m_il.mark_label(willJump);

				m_il.ld_loc(tmp);	// load the value back onto the stack
				m_il.branch(BranchAlways, getOffsetLabel(oparg));

				m_il.mark_label(noJump);

				// dec ref because we're popping...
				m_il.ld_loc(tmp);
				decref();

				m_il.free_local(tmp);
			}
				break;
			case POP_JUMP_IF_TRUE:
			case POP_JUMP_IF_FALSE:
			{
				auto noJump = m_il.define_label();
				auto willJump = m_il.define_label();
				// fast checks for true/false...
				m_il.dup();
				m_il.push_ptr(byte == POP_JUMP_IF_FALSE ? Py_True : Py_False);
				m_il.compare_eq();
				m_il.branch(BranchTrue, noJump);

				m_il.dup();
				m_il.push_ptr(byte == POP_JUMP_IF_FALSE ? Py_False : Py_True);
				m_il.compare_eq();
				m_il.branch(BranchTrue, willJump);

				// Use PyObject_IsTrue
				m_il.dup();
				m_il.emit_call(METHOD_PYOBJECT_ISTRUE);
				m_il.ld_i(0);
				m_il.compare_eq();
				m_il.branch(byte == POP_JUMP_IF_FALSE ? BranchFalse : BranchTrue, noJump);

				m_il.mark_label(willJump);
				decref();

				m_il.branch(BranchAlways, getOffsetLabel(oparg));

				m_il.mark_label(noJump);
				decref();
			}
				break;
			case LOAD_NAME:
				load_frame();
				m_il.push_ptr(PyTuple_GetItem(m_code->co_names, oparg));
				m_il.emit_call(METHOD_LOADNAME_TOKEN);
				check_error(i, "load name");
				break;
			case STORE_ATTR:
			{
				auto globalName = PyTuple_GetItem(m_code->co_names, oparg);
				m_il.push_ptr(globalName);
			}
				m_il.emit_call(METHOD_STOREATTR_TOKEN);
				check_int_error(i);
				break;
			case DELETE_ATTR:
			{
				auto globalName = PyTuple_GetItem(m_code->co_names, oparg);
				m_il.push_ptr(globalName);
			}
				m_il.emit_call(METHOD_DELETEATTR_TOKEN);
				check_int_error(i);
				break;
			case LOAD_ATTR:
			{
				auto globalName = PyTuple_GetItem(m_code->co_names, oparg);
				m_il.push_ptr(globalName);
			}
				m_il.emit_call(METHOD_LOADATTR_TOKEN);
				check_error(i, "load attr");
				break;
			case STORE_GLOBAL:
				// value is on the stack
				load_frame();
				{
					auto globalName = PyTuple_GetItem(m_code->co_names, oparg);
					m_il.push_ptr(globalName);
				}
				m_il.emit_call(METHOD_STOREGLOBAL_TOKEN);
				check_int_error(i);
				break;
			case DELETE_GLOBAL:
				load_frame();
				{
					auto globalName = PyTuple_GetItem(m_code->co_names, oparg);
					m_il.push_ptr(globalName);
				}
				m_il.emit_call(METHOD_DELETEGLOBAL_TOKEN);
				check_int_error(i);
				break;

			case LOAD_GLOBAL:
				load_frame();
				{
					auto globalName = PyTuple_GetItem(m_code->co_names, oparg);
					m_il.push_ptr(globalName);
				}
				m_il.emit_call(METHOD_LOADGLOBAL_TOKEN);
				check_error(i, "load global");
				break;
			case LOAD_CONST:
				m_il.push_ptr(PyTuple_GetItem(m_code->co_consts, oparg));
				m_il.dup();
				incref();
				break;
			case STORE_NAME:
				load_frame();
				m_il.push_ptr(PyTuple_GetItem(m_code->co_names, oparg));
				m_il.emit_call(METHOD_STORENAME_TOKEN);
				//check_int_error(i); // TODO: Enable me
				break;
			case DELETE_NAME:
				load_frame();
				m_il.push_ptr(PyTuple_GetItem(m_code->co_names, oparg));
				m_il.emit_call(METHOD_DELETENAME_TOKEN);
				//check_int_error(i); // TODO: Enable me
				break;
			case DELETE_FAST:
			{
				load_local(oparg);
				m_il.load_null();
				auto valueSet = m_il.define_label();

				m_il.branch(BranchNotEqual, valueSet);
				m_il.push_ptr(PyTuple_GetItem(m_code->co_varnames, oparg));
				m_il.emit_call(METHOD_UNBOUND_LOCAL);
				m_il.branch(BranchLeave, GetEHBlock().Raise);

				m_il.mark_label(valueSet);
				load_local(oparg);
				decref();
				load_frame();
				m_il.ld_i(offsetof(PyFrameObject, f_localsplus) + oparg * sizeof(size_t));
				m_il.add();
				m_il.load_null();
				m_il.st_ind_i();
				break;
			}
			case STORE_FAST:
				// TODO: Move locals out of the Python frame object and into real locals
			{
				auto valueTmp = m_il.define_local(Parameter(CORINFO_TYPE_NATIVEINT));
				m_il.st_loc(valueTmp);

				load_frame();
				m_il.ld_i(offsetof(PyFrameObject, f_localsplus) + oparg * sizeof(size_t));
				m_il.add();

				m_il.ld_loc(valueTmp);

				m_il.st_ind_i();

				m_il.free_local(valueTmp);
			}
				break;
			case LOAD_FAST:
				/* PyObject *value = GETLOCAL(oparg); */
			{
				load_local(oparg);

				auto valueSet = m_il.define_label();

				//// TODO: Remove this check for definitely assigned values (e.g. params w/ no dels, 
				//// locals that are provably assigned)
				m_il.dup();
				m_il.load_null();
				m_il.branch(BranchNotEqual, valueSet);

				m_il.pop();
				m_il.push_ptr(PyTuple_GetItem(m_code->co_varnames, oparg));
				m_il.emit_call(METHOD_UNBOUND_LOCAL);
				m_il.branch(BranchLeave, GetEHBlock().Raise);

				m_il.mark_label(valueSet);

				m_il.dup();
				incref();
			}
				break;
			case UNPACK_SEQUENCE:
			{
				auto valueTmp = m_il.define_local(Parameter(CORINFO_TYPE_NATIVEINT));
				m_il.st_loc(valueTmp); // save the sequence

				// load the iterable, the count, and our temporary 
				// storage if we need to iterate over the object.
				m_il.ld_loc(valueTmp);
				m_il.push_ptr((void*)oparg);
				m_il.ld_loc(m_sequenceLocals[i]);
				m_il.emit_call(METHOD_UNPACK_SEQUENCE_TOKEN);
				check_error(i, "unapack sequence");

				auto fastTmp = m_il.define_local(Parameter(CORINFO_TYPE_NATIVEINT));
				m_il.st_loc(fastTmp);

				//while (oparg--) {
				//	item = items[oparg];
				//	Py_INCREF(item);
				//	PUSH(item);
				//}

				auto tmpOpArg = oparg;
				while (tmpOpArg--) {
					m_il.ld_loc(fastTmp);
					m_il.push_ptr((void*)(tmpOpArg * sizeof(size_t)));
					m_il.add();
					m_il.ld_ind_i();
					m_il.dup();
					incref();
				}

				m_il.ld_loc(valueTmp);
				decref();

				m_il.free_local(valueTmp);
				m_il.free_local(fastTmp);
			}
				break;
			case UNPACK_EX:
			{
				auto valueTmp = m_il.define_local(Parameter(CORINFO_TYPE_NATIVEINT));
				auto listTmp = m_il.define_local(Parameter(CORINFO_TYPE_NATIVEINT));
				auto remainderTmp = m_il.define_local(Parameter(CORINFO_TYPE_NATIVEINT));
				m_il.st_loc(valueTmp); // save the sequence

				// load the iterable, the sizes, and our temporary 
				// storage if we need to iterate over the object, 
				// the list local address, and the remainder address
				// PyObject* seq, size_t leftSize, size_t rightSize, PyObject** tempStorage, PyObject** list, PyObject*** remainder

				m_il.ld_loc(valueTmp);
				m_il.push_ptr((void*)(oparg & 0xFF));
				m_il.push_ptr((void*)(oparg >> 8));
				m_il.ld_loc(m_sequenceLocals[i]);
				m_il.ld_loca(listTmp);
				m_il.ld_loca(remainderTmp);
				m_il.emit_call(METHOD_UNPACK_SEQUENCEEX_TOKEN);
				check_error(i, "unpack seq ex");

				auto fastTmp = m_il.define_local(Parameter(CORINFO_TYPE_NATIVEINT));
				m_il.st_loc(fastTmp);

				// load the right hand side...
				auto tmpOpArg = oparg >> 8;
				while (tmpOpArg--) {
					m_il.ld_loc(remainderTmp);
					m_il.push_ptr((void*)(tmpOpArg * sizeof(size_t)));
					m_il.add();
					m_il.ld_ind_i();
					m_il.dup();
					incref();
				}

				// load the list
				m_il.ld_loc(listTmp);

				// load the left hand side
				//while (oparg--) {
				//	item = items[oparg];
				//	Py_INCREF(item);
				//	PUSH(item);
				//}

				tmpOpArg = oparg & 0xff;
				while (tmpOpArg--) {
					m_il.ld_loc(fastTmp);
					m_il.push_ptr((void*)(tmpOpArg * sizeof(size_t)));
					m_il.add();
					m_il.ld_ind_i();
					m_il.dup();
					incref();
				}

				m_il.ld_loc(valueTmp);
				decref();

				m_il.free_local(valueTmp);
				m_il.free_local(fastTmp);
				m_il.free_local(remainderTmp);
				m_il.free_local(listTmp);
			}
				break;
			case CALL_FUNCTION_VAR:
			case CALL_FUNCTION_KW:
			case CALL_FUNCTION_VAR_KW:
			{
				int na = oparg & 0xff;
				int nk = (oparg >> 8) & 0xff;
				int flags = (byte - CALL_FUNCTION) & 3;
				int n = na + 2 * nk;
#define CALL_FLAG_VAR 1
#define CALL_FLAG_KW 2
				auto varArgs = m_il.define_local(Parameter(CORINFO_TYPE_NATIVEINT));
				auto varKwArgs = m_il.define_local(Parameter(CORINFO_TYPE_NATIVEINT));
				if (flags & CALL_FLAG_KW) {
					// kw args dict is last on the stack, save it....
					m_il.st_loc(varKwArgs);
				}

				Local map;
				if (nk != 0) {
					// if we have keywords build the map, and then save them...
					build_map(nk);
					map = m_il.define_local(Parameter(CORINFO_TYPE_NATIVEINT));
					m_il.st_loc(map);
				}

				if (flags & CALL_FLAG_VAR) {
					// then save var args...
					m_il.st_loc(varArgs);
				}

				// now we have the normal args (if any), and then the function
				// Build a tuple of the normal args...
				if (na != 0) {
					build_tuple(na);
				}
				else{
					m_il.load_null();
				}

				// If we have keywords load them or null
				if (nk != 0) {
					m_il.ld_loc(map);
				}
				else{
					m_il.load_null();
				}

				// If we have var args load them or null
				if (flags & CALL_FLAG_VAR) {
					m_il.ld_loc(varArgs);
				}
				else{
					m_il.load_null();
				}

				// If we have a kw dict, load it...
				if (flags & CALL_FLAG_KW) {
					m_il.ld_loc(varKwArgs);
				}
				else{
					m_il.load_null();
				}

				// finally emit the call to our helper...
				m_il.emit_call(METHOD_PY_FANCYCALL);
				check_error(i, "fancy call");

				m_il.free_local(varArgs);
				m_il.free_local(varKwArgs);
				if (nk != 0) {
					m_il.free_local(map);
				}
			}
				break;
			case CALL_FUNCTION:
			{
				int argCnt = oparg & 0xff;
				int kwArgCnt = (oparg >> 8) & 0xff;
				// Optimize for # of calls, and various call types...
				// Function is last thing on the stack...

				// target + args popped, result pushed
				if (kwArgCnt == 0) {
					switch (argCnt) {
					case 0: 
						m_il.emit_call(METHOD_CALL0_TOKEN); 
						check_error(i, "call 0"); break;
						/*
						case 1: emit_call(m_il, METHOD_CALL1_TOKEN); break;
						case 2: emit_call(m_il, METHOD_CALL2_TOKEN); break;
						case 3: emit_call(m_il, METHOD_CALL3_TOKEN); break;
						case 4: emit_call(m_il, METHOD_CALL4_TOKEN); break;
						case 5: emit_call(m_il, METHOD_CALL5_TOKEN); break;
						case 6: emit_call(m_il, METHOD_CALL6_TOKEN); break;
						case 7: emit_call(m_il, METHOD_CALL7_TOKEN); break;
						case 8: emit_call(m_il, METHOD_CALL8_TOKEN); break;
						case 9: emit_call(m_il, METHOD_CALL9_TOKEN); break;*/
					default:
						// generic call, build a tuple for the call...
						build_tuple(argCnt);

						// target is on the stack already...
						m_il.emit_call(METHOD_CALLN_TOKEN);
						check_error(i, "call n");
						break;
					}
				}
				else{
					build_map(kwArgCnt);
					auto map = m_il.define_local(Parameter(CORINFO_TYPE_NATIVEINT));
					m_il.st_loc(map);

					build_tuple(argCnt);
					m_il.ld_loc(map);
					m_il.emit_call(METHOD_CALLNKW_TOKEN);
					check_error(i, "call nkw");
					m_il.free_local(map);
					break;
				}
			}
				break;
			case BUILD_TUPLE:
				if (oparg == 0) {
					m_il.push_ptr(PyTuple_New(0));
				}
				else{
					build_tuple(oparg);
				}
				break;
			case BUILD_LIST:
				build_list(oparg);
				break;
			case BUILD_MAP:
				build_map(oparg);
				break;
#if PY_MINOR_VERSION == 5
			case STORE_MAP:
				return nullptr;
				// stack is map, key, value
				m_il.emit_call(METHOD_STOREMAP_TOKEN);
				break;
#endif
			case STORE_SUBSCR:
				// stack is value, container, index
				m_il.emit_call(METHOD_STORESUBSCR_TOKEN);
				check_int_error(i);
				break;
			case DELETE_SUBSCR:
				// stack is container, index
				m_il.emit_call(METHOD_DELETESUBSCR_TOKEN);
				check_int_error(i);
				break;
			case BUILD_SLICE:
				if (oparg != 3) {
					m_il.load_null();
				}
				m_il.emit_call(METHOD_BUILD_SLICE);
				break;
			case BUILD_SET:
				build_set(oparg);
				break;
			case UNARY_POSITIVE:
				m_il.emit_call(METHOD_UNARY_POSITIVE);
				break;
			case UNARY_NEGATIVE:
				m_il.emit_call(METHOD_UNARY_NEGATIVE);
				break;
			case UNARY_NOT: m_il.emit_call(METHOD_UNARY_NOT); check_error(i, "not"); break;
			case UNARY_INVERT: m_il.emit_call(METHOD_UNARY_INVERT); check_error(i, "invert");  break;
			case BINARY_SUBSCR: m_il.emit_call(METHOD_SUBSCR_TOKEN); check_error(i, "subscr"); break;
			case BINARY_ADD: m_il.emit_call(METHOD_ADD_TOKEN); check_error(i, "add"); break;
			case BINARY_TRUE_DIVIDE: m_il.emit_call(METHOD_DIVIDE_TOKEN); check_error(i, "true divide"); break;
			case BINARY_FLOOR_DIVIDE: m_il.emit_call(METHOD_FLOORDIVIDE_TOKEN); check_error(i, "floor divide"); break;
			case BINARY_POWER: m_il.emit_call(METHOD_POWER_TOKEN); check_error(i, "power"); break;
			case BINARY_MODULO: m_il.emit_call(METHOD_MODULO_TOKEN); check_error(i, "modulo"); break;
			case BINARY_MATRIX_MULTIPLY: m_il.emit_call(METHOD_MATRIX_MULTIPLY_TOKEN); check_error(i, "matrix multi"); break;
			case BINARY_LSHIFT: m_il.emit_call(METHOD_BINARY_LSHIFT_TOKEN); check_error(i, "lshift"); break;
			case BINARY_RSHIFT: m_il.emit_call(METHOD_BINARY_RSHIFT_TOKEN); check_error(i, "rshift"); break;
			case BINARY_AND: m_il.emit_call(METHOD_BINARY_AND_TOKEN); check_error(i, "and"); break;
			case BINARY_XOR: m_il.emit_call(METHOD_BINARY_XOR_TOKEN); check_error(i, "xor"); break;
			case BINARY_OR: m_il.emit_call(METHOD_BINARY_OR_TOKEN); check_error(i, "or"); break;
			case BINARY_MULTIPLY: m_il.emit_call(METHOD_MULTIPLY_TOKEN); check_error(i, "multiply"); break;
			case BINARY_SUBTRACT: m_il.emit_call(METHOD_SUBTRACT_TOKEN); check_error(i, "subtract"); break;
			case RETURN_VALUE:
			{
				m_il.st_loc(retValue);

				int clearEh = -1;
				bool inFinally = false;
				for (size_t i = m_blockStack.size() - 1; i != (-1); i--) {
					if (m_blockStack[i].Kind == SETUP_FINALLY) {
						// we need to run the finally before returning...
						m_blockStack.data()[i].Flags |= BLOCK_RETURNS;

						if (!inFinally) {
							// Only emit the store and branch to the inner most finally, but
							// we need to mark all finallys as being capable of being returned
							// through.
							inFinally = true;
							if (clearEh != -1) {
								unwind_eh(m_blockStack[clearEh].ExVars);
							}
							m_il.ld_i4(BLOCK_RETURNS);
							m_il.branch(BranchAlways, m_blockStack[i].ErrorTarget);
						}
					}
					else if (m_blockStack[i].Kind == POP_EXCEPT || m_blockStack[i].Kind == END_FINALLY) {
						// we need to restore the previous exception before we return
						if (clearEh == -1) {
							clearEh = i;
						}
					}
				}

				if (!inFinally) {
#ifdef DEBUG_TRACE
					char* tmp = (char*)malloc(100);
					sprintf_s(tmp, 100, "Returning %s...", PyUnicode_AsUTF8(m_code->co_name));
					m_il.push_ptr(tmp);
					m_il.emit_call(METHOD_DEBUG_TRACE);
#endif
					if (clearEh != -1) {
						unwind_eh(m_blockStack[clearEh].ExVars);
					}

					m_il.branch(BranchLeave, retLabel);
				}
			}
				break;
			case EXTENDED_ARG:
			{
				byte = m_byteCode[++i];
				int bottomArg = NEXTARG();
				oparg = (oparg << 16) | bottomArg;
				goto processOpCode;
			}
			case MAKE_CLOSURE:
			case MAKE_FUNCTION:
			{
				int posdefaults = oparg & 0xff;
				int kwdefaults = (oparg >> 8) & 0xff;
				int num_annotations = (oparg >> 16) & 0x7fff;

				load_frame();
				m_il.emit_call(METHOD_NEWFUNCTION_TOKEN);

				if (byte == MAKE_CLOSURE) {
					m_il.emit_call(METHOD_SET_CLOSURE);
				}
				if (num_annotations > 0 || kwdefaults > 0 || posdefaults > 0) {
					auto func = m_il.define_local(Parameter(CORINFO_TYPE_NATIVEINT));
					m_il.st_loc(func);
					if (num_annotations > 0) {
						// names is on stack, followed by values.
						//PyObject* values, PyObject* names, PyObject* func
						auto names = m_il.define_local(Parameter(CORINFO_TYPE_NATIVEINT));
						m_il.st_loc(names);

						// for whatever reason ceval.c has "assert(num_annotations == name_ix+1);", where
						// name_ix is the numbe of elements in the names tuple.  Otherwise num_annotations
						// goes unused!
						// TODO: If we hit an OOM here then build_tuple doesn't release the function
						build_tuple(num_annotations - 1);
						m_il.ld_loc(names);
						m_il.ld_loc(func);
						m_il.emit_call(METHOD_PY_FUNC_SET_ANNOTATIONS);
						check_int_error(i);
						m_il.free_local(names);
					}
					if (kwdefaults > 0) {
						// TODO: If we hit an OOM here then build_map doesn't release the function
						build_map(kwdefaults);
						m_il.ld_loc(func);
						m_il.emit_call(METHOD_PY_FUNC_SET_KW_DEFAULTS);
						check_int_error(i);
					}
					if (posdefaults > 0) {
						build_tuple(posdefaults);
						m_il.ld_loc(func);
						m_il.emit_call(METHOD_FUNC_SET_DEFAULTS);
						check_int_error(i);
					}
					m_il.ld_loc(func);
				}
				break;
			}
			case LOAD_DEREF:
				load_frame();
				m_il.ld_i4(oparg);
				//m_il.ld_i(offsetof(PyFrameObject, f_localsplus) + (m_code->co_nlocals + oparg) * sizeof(size_t));
				//m_il.add();
				//m_il.ld_ind_i();
				m_il.emit_call(METHOD_PYCELL_GET);
				check_error(i, "pycell get");
				break;
			case STORE_DEREF:
				load_frame();
				m_il.ld_i(offsetof(PyFrameObject, f_localsplus) + (m_code->co_nlocals + oparg) * sizeof(size_t));
				m_il.add();
				m_il.ld_ind_i();
				m_il.emit_call(METHOD_PYCELL_SET_TOKEN);
				break;
			case DELETE_DEREF:
				m_il.load_null();
				load_frame();
				m_il.ld_i(offsetof(PyFrameObject, f_localsplus) + (m_code->co_nlocals + oparg) * sizeof(size_t));
				m_il.add();
				m_il.ld_ind_i();
				m_il.emit_call(METHOD_PYCELL_SET_TOKEN);
				break;
			case LOAD_CLOSURE:
				load_frame();
				m_il.ld_i(offsetof(PyFrameObject, f_localsplus) + (m_code->co_nlocals + oparg) * sizeof(size_t));
				m_il.add();
				m_il.ld_ind_i();
				m_il.dup();
				incref();
				break;
			case GET_ITER:
				m_il.emit_call(METHOD_GETITER_TOKEN);
				break;
			case FOR_ITER:
			{
				// CPython always generates LOAD_FAST or a GET_ITER before a FOR_ITER.
				// Therefore we know that we always fall into a FOR_ITER when it is
				// initialized, and we branch back to it for the loop condition.  We
				// do this becaues keeping the value on the stack becomes problematic.
				// At the very least it requires that we spill the value out when we
				// are doing a "continue" in a for loop.

				// oparg is where to jump on break
				auto iterValue = m_il.define_local(Parameter(CORINFO_TYPE_NATIVEINT));
				m_il.st_loc(iterValue);		// store the value...

				// now that we've saved the value into a temp we can mark the offset
				// label.
				mark_offset_label(i - 2);	// minus 2 removes our oparg

				m_il.ld_loc(iterValue);

				/*
				// TODO: It'd be nice to inline this...
				m_il.dup();
				LD_FIELD(PyObject, ob_type);
				LD_FIELD(PyTypeObject, tp_iternext);*/
				//m_il.push_ptr((void*)offsetof(PyObject, ob_type));
				//m_il.push_back(CEE_ADD);
				auto error = m_il.define_local(Parameter(CORINFO_TYPE_INT));
				m_il.ld_loca(error);

				m_il.emit_call(SIG_ITERNEXT_TOKEN);
				auto processValue = m_il.define_label();
				m_il.dup();
				m_il.push_ptr(nullptr);
				m_il.compare_eq();
				m_il.branch(BranchFalse, processValue);

				// iteration has ended, or an exception was raised...
				m_il.pop();
				m_il.ld_loc(error);
				check_int_error(i);

				m_il.branch(BranchAlways, getOffsetLabel(i + oparg + 1));

				// leave iter and value on stack
				m_il.mark_label(processValue);
				m_il.free_local(error);
			}
				break;
			case SET_ADD:
			{
				// due to FOR_ITER magic we store the
				// iterable off the stack, and oparg here is based upon the stacking
				// of the generator indexes, so we don't need to spill anything...
				m_il.emit_call(METHOD_SET_ADD_TOKEN);
				check_error(i, "set add");
			}
				break;
			case MAP_ADD:
			{
				m_il.emit_call(METHOD_MAP_ADD_TOKEN);
				check_error(i, "map add");
			}
				break;
			case LIST_APPEND:
			{
				m_il.emit_call(METHOD_LIST_APPEND_TOKEN);
				check_error(i, "list append");
			}
				break;
			case INPLACE_POWER: m_il.emit_call(METHOD_INPLACE_POWER_TOKEN); check_error(i, "inplace power"); break;
			case INPLACE_MULTIPLY: m_il.emit_call(METHOD_INPLACE_MULTIPLY_TOKEN); check_error(i, "inplace multiply"); break;
			case INPLACE_MATRIX_MULTIPLY: m_il.emit_call(METHOD_INPLACE_MATRIX_MULTIPLY_TOKEN); check_error(i, "inplace matrix"); break;
			case INPLACE_TRUE_DIVIDE: m_il.emit_call(METHOD_INPLACE_TRUE_DIVIDE_TOKEN); check_error(i, "inplace true div"); break;
			case INPLACE_FLOOR_DIVIDE: m_il.emit_call(METHOD_INPLACE_FLOOR_DIVIDE_TOKEN); check_error(i, "inplace floor div"); break;
			case INPLACE_MODULO: m_il.emit_call(METHOD_INPLACE_MODULO_TOKEN); check_error(i, "inplace modulo"); break;
			case INPLACE_ADD:
				// TODO: We should do the unicode_concatenate ref count optimization
				m_il.emit_call(METHOD_INPLACE_ADD_TOKEN);
				check_error(i, "inplace add");
				break;
			case INPLACE_SUBTRACT: m_il.emit_call(METHOD_INPLACE_SUBTRACT_TOKEN); check_error(i, "inplace subtract"); break;
			case INPLACE_LSHIFT: m_il.emit_call(METHOD_INPLACE_LSHIFT_TOKEN); check_error(i, "inplace lshift"); break;
			case INPLACE_RSHIFT:m_il.emit_call(METHOD_INPLACE_RSHIFT_TOKEN); check_error(i, "inplace rshift"); break;
			case INPLACE_AND: m_il.emit_call(METHOD_INPLACE_AND_TOKEN); check_error(i, "inplace and"); break;
			case INPLACE_XOR:m_il.emit_call(METHOD_INPLACE_XOR_TOKEN); check_error(i, "inplace xor"); break;
			case INPLACE_OR: m_il.emit_call(METHOD_INPLACE_OR_TOKEN); check_error(i, "inplace or"); break;
			case PRINT_EXPR:
				m_il.emit_call(METHOD_PRINT_EXPR_TOKEN);
				check_int_error(i);
				break;
			case LOAD_CLASSDEREF:
				load_frame();
				m_il.ld_i(oparg);
				m_il.emit_call(METHOD_LOAD_CLASSDEREF_TOKEN);
				check_error(i, "class deref");
				break;
			case RAISE_VARARGS:
				// do raise (exception, cause)
				// We can be invoked with no args (bare raise), raise exception, or raise w/ exceptoin and cause
				switch (oparg) {
				case 0: m_il.load_null();
				case 1: m_il.load_null();
				case 2:
#ifdef DEBUG_TRACE
					char* tmp = (char*)malloc(100);
					sprintf_s(tmp, 100, "Exception explicitly raised in %s", PyUnicode_AsUTF8(m_code->co_name));
					m_il.push_ptr(tmp);
					m_il.emit_call(METHOD_DEBUG_TRACE);
#endif

					// raise exc
					m_il.emit_call(METHOD_DO_RAISE);
					// returns 1 if we're doing a re-raise in which case we don't need
					// to update the traceback.  Otherwise returns 0.
					auto curHandler = GetEHBlock();
					if (oparg == 0) {
						m_il.branch(BranchFalse, curHandler.Raise);
						m_il.branch(BranchAlways, curHandler.ReRaise);
					}
					else {
						// if we have args we'll always return 0...
						m_il.pop();
						m_il.branch(BranchAlways, curHandler.Raise);
					}
					break;
				}
				break;
			case SETUP_EXCEPT:
			{
				// TODO :: Need to save existing exception state...
				auto handlerLabel = getOffsetLabel(oparg + i + 1);
				auto blockInfo = BlockInfo(m_il.define_label(), m_il.define_label(), handlerLabel, oparg + i + 1, SETUP_EXCEPT);
				blockInfo.ExVars = ExceptionVars(
					m_il.define_local_no_cache(Parameter(CORINFO_TYPE_NATIVEINT)),
					m_il.define_local_no_cache(Parameter(CORINFO_TYPE_NATIVEINT)),
					m_il.define_local_no_cache(Parameter(CORINFO_TYPE_NATIVEINT))
				);
				m_blockStack.push_back(blockInfo);
				m_allHandlers.push_back(blockInfo);
				m_ehInfo.push_back(EhInfo(false));
			}
				break;
			case SETUP_FINALLY: {
				auto handlerLabel = getOffsetLabel(oparg + i + 1);
				auto blockInfo = BlockInfo(m_il.define_label(), m_il.define_label(), handlerLabel, oparg + i + 1, SETUP_FINALLY);
				blockInfo.ExVars = ExceptionVars(
					m_il.define_local_no_cache(Parameter(CORINFO_TYPE_NATIVEINT)),
					m_il.define_local_no_cache(Parameter(CORINFO_TYPE_NATIVEINT)),
					m_il.define_local_no_cache(Parameter(CORINFO_TYPE_NATIVEINT))
					);
				m_blockStack.push_back(blockInfo);
				m_allHandlers.push_back(blockInfo);
				m_ehInfo.push_back(EhInfo(true));
			}
				break;
			case POP_EXCEPT:
			{
				// we made it to the end of an EH block w/o throwing,
				// clear the exception.
				m_il.load_null();
				m_il.st_loc(ehVal);
				auto block = m_blockStack.back();
				//m_blockStack.pop_back();
				unwind_eh(block.ExVars);
#ifdef DEBUG_TRACE
				{
					char* tmp = (char*)malloc(100);
					sprintf_s(tmp, 100, "Exception cleared %d", i);
					m_il.push_ptr(tmp);
					m_il.emit_call(METHOD_DEBUG_TRACE);
				}
#endif
			}
				break;
			case POP_BLOCK:
			{
				auto curHandler = m_blockStack.back();				
				m_blockStack.pop_back();
				if (curHandler.Kind == SETUP_FINALLY || curHandler.Kind == SETUP_EXCEPT) {
					m_ehInfo.data()[m_ehInfo.size() - 1].Flags = curHandler.Flags;

					// convert block into an END_FINALLY BlockInfo which will
					// dispatch to all of the previous locations...
					auto back = m_blockStack.back();
					auto newBlock = BlockInfo(
						back.Raise,		// if we take a nested exception this is where we go to...
						back.ReRaise,
						back.ErrorTarget,
						back.EndOffset,
						curHandler.Kind == SETUP_FINALLY ? END_FINALLY : POP_EXCEPT,
						curHandler.Flags,
						curHandler.ContinueOffset
						);
					newBlock.ExVars = curHandler.ExVars;
					m_blockStack.push_back(newBlock);
				}
			}
				break;
			case END_FINALLY: {
				// CPython excepts END_FINALLY can be entered in 1 of 3 ways:
				//	1) With a status code for why the finally is unwinding, indicating a RETURN
				//			or a continue.  In this case there is an extra retval on the stack
				//	2) With an excpetion class which is being raised.  In this case there are 2 extra
				//			values on the stack, the exception value, and the traceback.
				//	3) After the try block has completed normally.  In this case None is on the stack.
				//
				//	That means in CPython this opcode can be branched to with 1 of 3 different stack
				//		depths, and the CLR doesn't like that.  Worse still the rest of the generated
				//		byte code assumes this is true.  For case 2 an except handler includes tests
				//		and pops which remove the 3 values from the class.  For case 3 the byte code
				//		at the end of the finally range includes the opcode to load None.
				//
				//  END_FINALLY can also be encountered w/o a SETUP_FINALLY, as happens when it's used
				//	solely for re-throwing exceptions.

				auto ehInfo = m_ehInfo.back();
				m_ehInfo.pop_back();
				auto exVars = m_blockStack.back().ExVars;
				m_blockStack.pop_back();

				if (ehInfo.IsFinally) {
					int flags = ehInfo.Flags;

					// restore the previous exception...
					unwind_eh(exVars);

					// We're actually ending a finally.  If we're in an exceptional case we
					// need to re-throw, otherwise we need to just continue execution.  Our
					// exception handling code will only push the exception type on in this case.
					auto finallyReason = m_il.define_local(Parameter(CORINFO_TYPE_NATIVEINT));
					auto noException = m_il.define_label();
					m_il.st_loc(finallyReason);
					m_il.ld_loc(finallyReason);
					m_il.push_ptr(Py_None);
					m_il.branch(BranchEqual, noException);

					if (flags & BLOCK_BREAKS) {
						for (size_t i = m_blockStack.size() - 1; i != -1; i--) {
							if (m_blockStack[i].Kind == SETUP_LOOP) {
								m_il.ld_loc(finallyReason);
								m_il.ld_i(BLOCK_BREAKS);
								m_il.branch(BranchEqual, getOffsetLabel(m_blockStack[i].EndOffset));
								break;
							}
							else if (m_blockStack[i].Kind == SETUP_FINALLY) {
								// need to dispatch to outer finally...
								m_il.ld_loc(finallyReason);
								m_il.branch(BranchAlways, m_blockStack[i].ErrorTarget);
								break;
							}
						}
					}

					if (flags & BLOCK_CONTINUES) {
						for (size_t i = m_blockStack.size() - 1; i != -1; i--) {
							if (m_blockStack[i].Kind == SETUP_LOOP) {
								m_il.ld_loc(finallyReason);
								m_il.ld_i(BLOCK_CONTINUES);
								m_il.branch(BranchEqual, getOffsetLabel(m_blockStack[i].ContinueOffset));
								break;
							}
							else if (m_blockStack[i].Kind == SETUP_FINALLY) {
								// need to dispatch to outer finally...
								m_il.ld_loc(finallyReason);
								m_il.branch(BranchAlways, m_blockStack[i].ErrorTarget);
								break;
							}
						}
					}

					if (flags & BLOCK_RETURNS) {
						auto exceptional = m_il.define_label();
						m_il.ld_loc(finallyReason);
						m_il.ld_i(BLOCK_RETURNS);
						m_il.compare_eq();
						m_il.branch(BranchFalse, exceptional);

						bool hasOuterFinally = false;
						for (size_t i = m_blockStack.size() - 1; i != -1; i--) {
							if (m_blockStack[i].Kind == SETUP_FINALLY) {
								// need to dispatch to outer finally...
								m_il.ld_loc(finallyReason);
								m_il.branch(BranchAlways, m_blockStack[i].ErrorTarget);
								hasOuterFinally = true;
								break;
							}
						}
						if (!hasOuterFinally) {
							m_il.branch(BranchLeave, retLabel);
						}

						m_il.mark_label(exceptional);
					}

					// re-raise the exception...
					m_il.ld_loc(tb);
					m_il.ld_loc(ehVal);
					m_il.ld_loc(finallyReason);
					m_il.emit_call(METHOD_PYERR_RESTORE);
					m_il.branch(BranchAlways, GetEHBlock().ReRaise);

					m_il.mark_label(noException);
#ifdef DEBUG_TRACE
					m_il.push_ptr("finally exited normally...");
					m_il.emit_call(METHOD_DEBUG_TRACE);
#endif

					m_il.free_local(finallyReason);
				}
				else{
					// END_FINALLY is marking the EH rethrow.  The byte code branches
					// around this in the non-exceptional case.
					m_il.setStackDepth(3);
					m_il.emit_call(METHOD_PYERR_RESTORE);
					m_il.branch(BranchAlways, GetEHBlock().ReRaise);
				}
			}
				break;

			case YIELD_FROM:
			case YIELD_VALUE:
				printf("Unsupported opcode: %d (yield related)\r\n", byte);
				//_ASSERT(FALSE);
				return nullptr;

			case IMPORT_NAME:
				m_il.push_ptr(PyTuple_GetItem(m_code->co_names, oparg));
				load_frame();
				m_il.emit_call(METHOD_PY_IMPORTNAME);
				check_error(i, "import name");
				break;
			case IMPORT_FROM:
				m_il.dup();
				m_il.push_ptr(PyTuple_GetItem(m_code->co_names, oparg));
				m_il.emit_call(METHOD_PY_IMPORTFROM);
				check_error(i, "import from");
				break;
			case IMPORT_STAR:
				load_frame();
				m_il.emit_call(METHOD_PY_IMPORTSTAR);
				check_int_error(i);
				break;
			case SETUP_WITH:
			case WITH_CLEANUP_START:
			case WITH_CLEANUP_FINISH:
			default:
				printf("Unsupported opcode: %d (with related)\r\n", byte);
				//_ASSERT(FALSE);
				return nullptr;
			}


		}


		// for each exception handler we need to load the exception
		// information onto the stack, and then branch to the correct
		// handler.  When we take an error we'll branch down to this
		// little stub and then back up to the correct handler.
		if (m_allHandlers.size() != 0) {
			for (int i = 0; i < m_allHandlers.size(); i++) {
				m_il.mark_label(m_allHandlers[i].Raise);
#ifdef DEBUG_TRACE
				m_il.push_ptr("Exception raised");
				m_il.emit_call(METHOD_DEBUG_TRACE);
#endif

				load_frame();
				m_il.emit_call(METHOD_EH_TRACE);

				m_il.mark_label(m_allHandlers[i].ReRaise);
#ifdef DEBUG_TRACE
				m_il.push_ptr("Exception reraised");
				m_il.emit_call(METHOD_DEBUG_TRACE);
#endif
				
				m_il.ld_loca(excType);
				m_il.ld_loca(ehVal);
				m_il.ld_loca(tb);

				m_il.ld_loca(m_allHandlers[i].ExVars.PrevExc);
				m_il.ld_loca(m_allHandlers[i].ExVars.PrevExcVal);
				m_il.ld_loca(m_allHandlers[i].ExVars.PrevTraceback);

				m_il.emit_call(METHOD_PREPARE_EXCEPTION);
				if (m_allHandlers[i].Kind != SETUP_FINALLY) {
					m_il.ld_loc(tb);
					m_il.ld_loc(ehVal);
				}
				m_il.ld_loc(excType);
				m_il.branch(BranchAlways, m_allHandlers[i].ErrorTarget);
			}
		}

		// label we branch to for error handling when we have no EH handlers, return NULL.
		m_il.mark_label(raiseLabel);
#ifdef DEBUG_TRACE
		m_il.push_ptr("End raise exception ");
		m_il.emit_call(METHOD_DEBUG_TRACE);

		load_frame();
		m_il.emit_call(METHOD_EH_TRACE);
#endif
		m_il.mark_label(reraiseLabel);

#ifdef DEBUG_TRACE
		char* tmp = (char*)malloc(100);
		sprintf_s(tmp, 100, "Re-raising exception %s", PyUnicode_AsUTF8(m_code->co_name));
		m_il.push_ptr(tmp);
		m_il.emit_call(METHOD_DEBUG_TRACE);
#endif

		m_il.load_null();
		auto finalRet = m_il.define_label();
		m_il.branch(BranchAlways, finalRet);

		m_il.mark_label(retLabel);
		m_il.ld_loc(retValue);

		m_il.mark_label(finalRet);
		load_frame();
		m_il.emit_call(METHOD_PY_POPFRAME);
		m_il.emit_call(METHOD_PY_CHECKFUNCTIONRESULT);
		m_il.ret();
		
		return m_il.compile(&g_corJitInfo, g_jit, m_code->co_stacksize + 100).m_addr;
	}

	void debugLog(const char* fmt, va_list args) {
	}

};

extern "C" __declspec(dllexport) PVOID JitCompile(PyCodeObject* code) {
	if (strcmp(PyUnicode_AsUTF8(code->co_name), "<module>") == 0) {
		// TODO: Remove me, currently we can't compile encodings\aliases.py.
		return nullptr;
	}
#ifdef DEBUG_TRACE
	static int compileCount = 0, failCount = 0;
	printf("Compiling %s from %s line %d #%d (%d failures so far)\r\n", 
		PyUnicode_AsUTF8(code->co_name), 
		PyUnicode_AsUTF8(code->co_filename), 
		code->co_firstlineno,
		++compileCount,
		failCount);
#endif
	Jitter jitter(code);
	auto res = jitter.Compile();
#ifdef DEBUG_TRACE
	if (res == nullptr) {
		printf("Compilation failure #%d\r\n", ++failCount);
	}
#endif
	return res;
}

//VTableInfo g_iterNextVtable{ 2, { offsetof(PyObject, ob_type), offsetof(PyTypeObject, tp_iternext) } };

extern "C" __declspec(dllexport) void JitInit() {
	CeeInit();

	g_jit = getJit();

	g_emptyTuple = PyTuple_New(0);

	g_module = new Module();
	g_module->m_methods[METHOD_ADD_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_Add
		);
	g_module->m_methods[METHOD_SUBSCR_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_Subscr
		);
	g_module->m_methods[METHOD_MULTIPLY_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_Multiply
		);
	g_module->m_methods[METHOD_DIVIDE_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_TrueDivide
		);
	g_module->m_methods[METHOD_FLOORDIVIDE_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_FloorDivide
		);
	g_module->m_methods[METHOD_POWER_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_Power
		);
	g_module->m_methods[METHOD_MODULO_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_Modulo
		);
	g_module->m_methods[METHOD_SUBTRACT_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_Subtract
		);


	g_module->m_methods[METHOD_MATRIX_MULTIPLY_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_MatrixMultiply
		);
	g_module->m_methods[METHOD_BINARY_LSHIFT_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_BinaryLShift
		);
	g_module->m_methods[METHOD_BINARY_RSHIFT_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_BinaryRShift
		);
	g_module->m_methods[METHOD_BINARY_AND_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_BinaryAnd
		);
	g_module->m_methods[METHOD_BINARY_XOR_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_BinaryXor
		);
	g_module->m_methods[METHOD_BINARY_OR_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_BinaryOr
		);


	g_module->m_methods[METHOD_PYLIST_NEW] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyList_New
		);

	g_module->m_methods[METHOD_STOREMAP_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_INT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_StoreMap
		);
	g_module->m_methods[METHOD_STORESUBSCR_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_INT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_StoreSubscr
		);
	g_module->m_methods[METHOD_DELETESUBSCR_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_INT,
		std::vector < Parameter > { Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_DeleteSubscr
		);
	g_module->m_methods[METHOD_PYDICT_NEWPRESIZED] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT) },
		&_PyDict_NewPresized
		);
	g_module->m_methods[METHOD_PYTUPLE_NEW] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyTuple_New
		);
	g_module->m_methods[METHOD_PYSET_NEW] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT) },
		&PySet_New
		);
	g_module->m_methods[METHOD_PYOBJECT_ISTRUE] = Method(
		nullptr,
		CORINFO_TYPE_INT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyObject_IsTrue
		);

	g_module->m_methods[METHOD_PYITER_NEXT] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyIter_Next
		);

	g_module->m_methods[METHOD_PYCELL_GET] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_CellGet
		);

	g_module->m_methods[METHOD_RICHCMP_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_INT) },
		&PyJit_RichCompare
		);
	g_module->m_methods[METHOD_CONTAINS_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_Contains
		);
	g_module->m_methods[METHOD_NOTCONTAINS_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_NotContains
		);

	g_module->m_methods[METHOD_NEWFUNCTION_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_NewFunction
		);

	g_module->m_methods[METHOD_GETBUILDCLASS_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > { Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_BuildClass
		);

	g_module->m_methods[METHOD_UNPACK_SEQUENCE_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > { Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_UnpackSequence
		);

	g_module->m_methods[METHOD_UNPACK_SEQUENCEEX_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > { Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT),
		Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT)},
		&PyJit_UnpackSequenceEx
		);

	g_module->m_methods[METHOD_PYSET_ADD] = Method(
		nullptr,
		CORINFO_TYPE_INT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PySet_Add
		);
	g_module->m_methods[METHOD_CALL0_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT) },
		&Call0
		);
	
	g_module->m_methods[METHOD_CALLN_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_CallN
		);
	g_module->m_methods[METHOD_CALLNKW_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_CallNKW
		);
	g_module->m_methods[METHOD_STOREGLOBAL_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_INT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_StoreGlobal
		);
	g_module->m_methods[METHOD_DELETEGLOBAL_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_INT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_DeleteGlobal
		);
	g_module->m_methods[METHOD_LOADGLOBAL_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_LoadGlobal
		);
	g_module->m_methods[METHOD_LOADATTR_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_LoadAttr
		);
	g_module->m_methods[METHOD_STOREATTR_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_INT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_StoreAttr
		);
	g_module->m_methods[METHOD_DELETEATTR_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_INT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_DeleteAttr
		);
	g_module->m_methods[METHOD_LOADNAME_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_LoadName
		);
	g_module->m_methods[METHOD_STORENAME_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_VOID,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT)  },
		&PyJit_StoreName
		);
	g_module->m_methods[METHOD_DELETENAME_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_VOID,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT)  },
		&PyJit_DeleteName
		);
	g_module->m_methods[METHOD_GETITER_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > { Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_GetIter
		);
	g_module->m_methods[SIG_ITERNEXT_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > { Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_IterNext
		);
	g_module->m_methods[METHOD_DECREF_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_VOID,
		std::vector < Parameter > { Parameter(CORINFO_TYPE_NATIVEINT) },
		&Py_DecRef
		);
	g_module->m_methods[METHOD_PYCELL_SET_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_VOID,
		std::vector < Parameter > { Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_CellSet
		);
	g_module->m_methods[METHOD_SET_CLOSURE] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > { Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_SetClosure
		);
	g_module->m_methods[METHOD_BUILD_SLICE] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > { Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_BuildSlice
		);
	g_module->m_methods[METHOD_UNARY_POSITIVE] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > { Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_UnaryPositive
		);
	g_module->m_methods[METHOD_UNARY_NEGATIVE] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > { Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_UnaryNegative
		);
	g_module->m_methods[METHOD_UNARY_NOT] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > { Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_UnaryNot
		);

	g_module->m_methods[METHOD_UNARY_INVERT] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > { Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_UnaryInvert
		);

	g_module->m_methods[METHOD_LIST_APPEND_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > { Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_ListAppend
		);
	g_module->m_methods[METHOD_SET_ADD_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > { Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_SetAdd
		);

	g_module->m_methods[METHOD_MAP_ADD_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > { Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_MapAdd
		);

	g_module->m_methods[METHOD_INPLACE_POWER_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_InplacePower
		);

	g_module->m_methods[METHOD_INPLACE_MULTIPLY_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_InplaceMultiply
		);

	g_module->m_methods[METHOD_INPLACE_MATRIX_MULTIPLY_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_InplaceMatrixMultiply
		);

	g_module->m_methods[METHOD_INPLACE_TRUE_DIVIDE_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_InplaceTrueDivide
		);

	g_module->m_methods[METHOD_INPLACE_FLOOR_DIVIDE_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_InplaceFloorDivide
		);

	g_module->m_methods[METHOD_INPLACE_MODULO_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_InplaceModulo
		);

	g_module->m_methods[METHOD_INPLACE_ADD_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_InplaceAdd
		);

	g_module->m_methods[METHOD_INPLACE_SUBTRACT_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_InplaceSubtract
		);

	g_module->m_methods[METHOD_INPLACE_LSHIFT_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_InplaceLShift
		);

	g_module->m_methods[METHOD_INPLACE_RSHIFT_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_InplaceRShift
		);

	g_module->m_methods[METHOD_INPLACE_AND_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_InplaceAnd
		);

	g_module->m_methods[METHOD_INPLACE_XOR_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_InplaceXor
		);

	g_module->m_methods[METHOD_INPLACE_OR_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_InplaceOr
		);

	g_module->m_methods[METHOD_PRINT_EXPR_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_INT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_PrintExpr
		);

	g_module->m_methods[METHOD_LOAD_CLASSDEREF_TOKEN] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_LoadClassDeref
		);

	g_module->m_methods[METHOD_PREPARE_EXCEPTION] = Method(
		nullptr,
		CORINFO_TYPE_VOID,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT),
			Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT)
		},
		&PyJit_PrepareException
		);

	g_module->m_methods[METHOD_DO_RAISE] = Method(
		nullptr,
		CORINFO_TYPE_INT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_Raise
		);

	g_module->m_methods[METHOD_EH_TRACE] = Method(
		nullptr,
		CORINFO_TYPE_VOID,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_EhTrace
		);

	g_module->m_methods[METHOD_COMPARE_EXCEPTIONS] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_CompareExceptions
		);

	g_module->m_methods[METHOD_UNBOUND_LOCAL] = Method(
		nullptr,
		CORINFO_TYPE_VOID,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_UnboundLocal
		);

	g_module->m_methods[METHOD_PYERR_RESTORE] = Method(
		nullptr,
		CORINFO_TYPE_VOID,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_PyErrRestore
		);

	g_module->m_methods[METHOD_DEBUG_TRACE] = Method(
		nullptr,
		CORINFO_TYPE_VOID,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_DebugTrace
		);
	
	g_module->m_methods[METHOD_FUNC_SET_DEFAULTS] = Method(
		nullptr,
		CORINFO_TYPE_INT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_FunctionSetDefaults
		);

	g_module->m_methods[METHOD_DEBUG_DUMP_FRAME] = Method(
		nullptr,
		CORINFO_TYPE_VOID,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_DebugDumpFrame
		);

	g_module->m_methods[METHOD_PY_POPFRAME] = Method(
		nullptr,
		CORINFO_TYPE_VOID,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_PopFrame
		);

	g_module->m_methods[METHOD_PY_PUSHFRAME] = Method(
		nullptr,
		CORINFO_TYPE_VOID,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT) },
		&PyJit_PushFrame
		);

	g_module->m_methods[METHOD_UNWIND_EH] = Method(
		nullptr,
		CORINFO_TYPE_VOID,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT)},
		&PyJit_UnwindEh
		);

	g_module->m_methods[METHOD_PY_CHECKFUNCTIONRESULT] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT)},
		&PyJit_CheckFunctionResult
		);
	
	g_module->m_methods[METHOD_PY_IMPORTNAME] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT)},
		&PyJit_ImportName
		);

	
	g_module->m_methods[METHOD_PY_FANCYCALL] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT),
								Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT)},
		&PyJit_FancyCall
	);

	g_module->m_methods[METHOD_PY_IMPORTFROM] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT)},
		&PyJit_ImportFrom
		);
	g_module->m_methods[METHOD_PY_IMPORTSTAR] = Method(
		nullptr,
		CORINFO_TYPE_NATIVEINT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT)},
		&PyJit_ImportStar
		);
	g_module->m_methods[METHOD_PY_FUNC_SET_ANNOTATIONS] = Method(
		nullptr,
		CORINFO_TYPE_INT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT)},
		&PyJit_FunctionSetAnnotations
		);	

	g_module->m_methods[METHOD_PY_FUNC_SET_KW_DEFAULTS] = Method(
		nullptr,
		CORINFO_TYPE_INT,
		std::vector < Parameter > {Parameter(CORINFO_TYPE_NATIVEINT), Parameter(CORINFO_TYPE_NATIVEINT)},
		&PyJit_FunctionSetKwDefaults
		);
	
	//g_module->m_methods[SIG_ITERNEXT_TOKEN] = Method(
	//	nullptr,
	//	CORINFO_TYPE_NATIVEINT,
	//	std::vector < Parameter > { },
	//	nullptr,
	//	&g_iterNextVtable
	//);
}
