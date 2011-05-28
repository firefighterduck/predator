/*
 * Copyright (C) 2010 Kamil Dudka <kdudka@redhat.com>
 *
 * This file is part of predator.
 *
 * predator is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * predator is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with predator.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"
#include "symcall.hh"

#include <cl/cl_msg.hh>
#include <cl/storage.hh>

#include "symbt.hh"
#include "symcut.hh"
#include "symheap.hh"
#include "symproc.hh"
#include "symstate.hh"
#include "symutil.hh"
#include "util.hh"

#include <vector>

#include <boost/foreach.hpp>

#if SE_ABSTRACT_ON_CALL_DONE
#   include "symabstract.hh"
#endif

// /////////////////////////////////////////////////////////////////////////////
// implementation of SymCallCtx
struct SymCallCtx::Private {
    SymBackTrace                *bt;
    const CodeStorage::Fnc      *fnc;
    SymHeap                     heap;
    SymHeap                     surround;
    const struct cl_operand     *dst;
    SymStateWithJoin            rawResults;
    int                         nestLevel;
    bool                        computed;
    bool                        flushed;

    void assignReturnValue(SymHeap &sh);
    void destroyStackFrame(SymHeap &sh);

    Private(TStorRef stor):
        heap(stor),
        surround(stor)
    {
    }
};

SymCallCtx::SymCallCtx(TStorRef stor):
    d(new Private(stor))
{
    d->computed = false;
    d->flushed = false;
}

SymCallCtx::~SymCallCtx() {
    delete d;
}

bool SymCallCtx::needExec() const {
    return !d->computed;
}

const SymHeap& SymCallCtx::entry() const {
    return d->heap;
}

SymState& SymCallCtx::rawResults() {
    return d->rawResults;
}

void SymCallCtx::Private::assignReturnValue(SymHeap &sh) {
    const cl_operand &op = *this->dst;
    if (CL_OPERAND_VOID == op.code)
        // we're done for a function returning void
        return;

    // wait, we're crossing stack frame boundaries here!  We need to use one
    // backtrace instance for source operands and another one for destination
    // operands.  The called function already appears on the given backtrace, so
    // that we can get the source backtrace by removing it from there locally.
    SymBackTrace callerSiteBt(*this->bt);
    callerSiteBt.popCall();
    SymProc proc(sh, &callerSiteBt);
    proc.setLocation(&op.data.var->loc);

    const TObjId obj = proc.objByOperand(op);
    CL_BREAK_IF(OBJ_INVALID == obj);

    const TValId val = sh.valueOf(sh.objAt(VAL_ADDR_OF_RET, op.type));
    CL_BREAK_IF(VAL_INVALID == val);

    // assign the return value in the current symbolic heap
    proc.objSetValue(obj, val);
}

void SymCallCtx::Private::destroyStackFrame(SymHeap &sh) {
    using namespace CodeStorage;
    const Fnc &ref = *this->fnc;
    SymProc proc(sh, this->bt);

    // gather live variables
    TCVarList liveVars;
    sh.gatherCVars(liveVars);

    // filter local variables
    TCVarList liveLocals;
    BOOST_FOREACH(const CVar &cv, liveVars) {
        if (!isOnStack(ref.stor->vars[cv.uid]))
            // not a local variable
            continue;

        if (cv.inst != this->nestLevel)
            // not a local variable in this context
            continue;

        if (hasKey(ref.vars, cv.uid))
            liveLocals.push_back(cv);
    }

    CL_DEBUG_MSG(&ref.def.data.cst.data.cst_fnc.loc,
            "<<< destroying stack frame of " << nameOf(ref) << "()"
            << ", nestLevel = " << this->nestLevel
            << ", varsTotal = " << ref.vars.size()
            << ", varsAlive = " << liveLocals.size());

    BOOST_FOREACH(const CVar &cv, liveLocals) {
        const Var &var = ref.stor->vars[cv.uid];
        const struct cl_loc *lw = &var.loc;
#if DEBUG_SE_STACK_FRAME
        CL_DEBUG_MSG(lw, "<<< destroying stack variable: #"
                << var.uid << " (" << var.name << ")" );
#endif
        proc.setLocation(lw);
        proc.valDestroyTarget(sh.addrOfVar(cv));
    }

    // We need to look for junk since there can be a function returning an
    // allocated object.  Then ignoring the return value on the caller's
    // side can trigger a memory leak.  See data/test-0090.c for a use case.
    proc.valDestroyTarget(VAL_ADDR_OF_RET);
}

void SymCallCtx::flushCallResults(SymState &dst) {
    if (d->flushed)
        // are we really ready for this?
        CL_TRAP;

    // mark as done
    d->computed = true;
    d->flushed = true;

    // go through the results and make them of the form that the caller likes
    const unsigned cnt = d->rawResults.size();
    for (unsigned i = 0; i < cnt; ++i) {
        if (1 < cnt) {
            CL_DEBUG("*** SymCallCtx::flushCallResults() is processing heap #"
                     << i);
        }

        // first join the heap with its original surround
        SymHeap sh(d->rawResults[i]);
        joinHeapsByCVars(&sh, &d->surround);

        // perform all necessary action wrt. our function call convention
        d->assignReturnValue(sh);
        d->destroyStackFrame(sh);

#if SE_ABSTRACT_ON_CALL_DONE
        // after the final merge and cleanup, chances are that the abstraction
        // may be useful
        abstractIfNeeded(sh);
#endif
        // flush the result
        dst.insert(sh);
    }
}

void SymCallCtx::invalidate() {
#if SE_DISABLE_CALL_CACHE
    delete this;
#endif
}

// /////////////////////////////////////////////////////////////////////////////
// call context cache per one fnc
class PerFncCache {
    private:
        typedef std::vector<SymCallCtx *> TCtxMap;

        SymHeapUnion    huni_;
        TCtxMap         ctxMap_;

    public:
        ~PerFncCache() {
            BOOST_FOREACH(SymCallCtx *ctx, ctxMap_) {
                delete ctx;
            }
        }

        /**
         * look for the given heap; return the corresponding call ctx if found,
         * 0 otherwise
         */
        SymCallCtx* lookup(const SymHeap &heap) {
#if SE_DISABLE_CALL_CACHE
            return 0;
#endif
            int idx = huni_.lookup(heap);
            if (-1 == idx)
                return 0;

            return ctxMap_.at(idx);
        }

        /**
         * store the given heap with its corresponding call ctx into the cache
         */
        void insert(const SymHeap &heap, SymCallCtx *ctx) {
#if SE_DISABLE_CALL_CACHE
            return;
#endif
            huni_.insertNew(heap);
            ctxMap_.push_back(ctx);
            CL_BREAK_IF(huni_.size() != ctxMap_.size());
        }
};

// /////////////////////////////////////////////////////////////////////////////
// implementation of SymCallCache
struct SymCallCache::Private {
    typedef SymBackTrace::TFncSeq                       TFncSeq;
    typedef std::map<int /* uid */, PerFncCache>        TCacheCol;
    typedef std::map<TCVarList, TCacheCol>              TCacheRow;
    typedef std::map<TFncSeq, TCacheRow>                TCache;

    TCache                      cache;
    SymBackTrace                *bt;
    const struct cl_loc         *lw;
    SymHeap                     *heap;
    SymProc                     *proc;
    const CodeStorage::Fnc      *fnc;
    int                         nestLevel;

    TCVarList                   glVars;

    void createStackFrame(TCVarList &vars);
    void setCallArgs(const CodeStorage::TOperandList &opList);
    SymCallCtx* getCallCtx(int uid, const SymHeap &heap);
};

SymCallCache::SymCallCache(SymBackTrace *bt):
    d(new Private)
{
    using namespace CodeStorage;
    d->bt = bt;

    // gather all gl variables
    const Storage &stor = bt->stor();
    BOOST_FOREACH(const Var &var, stor.vars) {
        if (VAR_GL == var.code) {
            const CVar cVar(var.uid, /* gl variable */ 0);
            d->glVars.push_back(cVar);
        }
    }

    // count gl variables
    const unsigned found = d->glVars.size();
    if (found)
        CL_DEBUG("(g) SymCallCache found " << found << " gl variable(s)");
}

SymCallCache::~SymCallCache() {
    delete d;
}

void SymCallCache::Private::createStackFrame(TCVarList &cVars) {
    using namespace CodeStorage;
    const Fnc &ref = *this->fnc;

#if DEBUG_SE_STACK_FRAME
    CL_DEBUG_MSG(this->lw,
            ">>> creating stack frame for " << nameOf(ref) << "()"
            << ", nestLevel = " << this->nestLevel);
#endif

    // go through all variables that are visible from the function
    BOOST_FOREACH(const int uid, ref.vars) {
        const Var &var = ref.stor->vars[uid];
        if (!isOnStack(var))
            continue;

        // gather local variables so that we can prune the heap afterwards
        const CVar cv(var.uid, this->nestLevel);
        cVars.push_back(cv);

        if (!var.initials.empty()) {
            CL_DEBUG_MSG(lw, "--- initializing stack variable: #" << var.uid
                    << " (" << var.name << ")" );

            // reflect the given initializer
            initVariable(*this->heap, *this->bt, var);
        }
    }
}

void SymCallCache::Private::setCallArgs(const CodeStorage::TOperandList &opList)
{
    // get called fnc's args
    const CodeStorage::TArgByPos &args = this->fnc->args;
    if (args.size() + 2 < opList.size()) {
        CL_DEBUG_MSG(this->lw,
                "too many arguments given (vararg fnc involved?)");

        const struct cl_loc *lwDecl = &this->fnc->def.data.cst.data.cst_fnc.loc;
        CL_DEBUG_MSG(lwDecl, "note: fnc was declared here");
    }

    // wait, we're crossing stack frame boundaries here!  We need to use one
    // backtrace instance for source operands and another one for destination
    // operands.  The called function already appears on the given backtrace, so
    // that we can get the source backtrace by removing it from there locally.
    SymBackTrace callerSiteBt(*this->bt);
    callerSiteBt.popCall();
    SymProc srcProc(*this->heap, &callerSiteBt);

    // initialize location info
    srcProc.setLocation(this->lw);
    this->proc->setLocation(this->lw);

    // set args' values
    unsigned pos = /* dst + fnc */ 2;
    BOOST_FOREACH(int arg, args) {

        // cVar lookup
        const CVar cVar(arg, this->nestLevel);
        const TObjId lhs = this->heap->objAt(this->heap->addrOfVar(cVar));
        CL_BREAK_IF(OBJ_INVALID == lhs);

        if (opList.size() <= pos) {
            // no value given for this arg
            CL_DEBUG_MSG(this->lw,
                    "missing argument being treated as unknown value");

            // read the UV_UNINITIALIZED of lhs
            TValId val = this->heap->valueOf(lhs);
            CL_BREAK_IF(VT_UNKNOWN != this->heap->valTarget(val));

            // change UV_UNINITIALIZED to UV_UNKNOWN in lhs
            val = this->heap->valCreate(VT_UNKNOWN, VO_UNKNOWN);
            this->proc->objSetValue(lhs, val);
            continue;
        }

        // read the given argument's value
        const struct cl_operand &op = opList[pos++];
        const TValId val = srcProc.valFromOperand(op);
        CL_BREAK_IF(VAL_INVALID == val);

        // set the value of lhs accordingly
        this->proc->objSetValue(lhs, val);
    }
}

SymCallCtx* SymCallCache::Private::getCallCtx(int uid, const SymHeap &heap) {
    TFncSeq seq;
    if (this->bt->hasRecursiveCall())
        // FIXME: this seems to be a silly limitation -- we require the call
        // sequence to match as long as there is any recursion involved
        seq = this->bt->getFncSequence();

    // 1st level cache lookup
    TCacheRow &row = this->cache[seq];

    // 2nd level cache lookup
    // FIXME: We rely on the assumption that gatherCVars() returns the variables
    //        always in the same order, however SymHeap API does not
    //        guarantee anything like that.  Luckily it should cause nothing
    //        evil in the analysis, only some unnecessary bloat of cache...
    TCVarList cVars;
    heap.gatherCVars(cVars);
    TCacheCol &col = row[cVars];

    // 3rd level cache lookup
    PerFncCache &pfc = col[uid];
    SymCallCtx *ctx = pfc.lookup(heap);
    if (!ctx) {
        // cache miss
        ctx = new SymCallCtx(heap.stor());
        ctx->d->bt      = this->bt;
        ctx->d->fnc     = this->fnc;
        ctx->d->heap    = heap;
        pfc.insert(heap, ctx);
        return ctx;
    }

    // cache hit, perform some sanity checks
    if (!ctx->d->computed) {
        // oops, we are not ready for this!
        CL_ERROR("SymCallCache: cache entry found, but result not computed yet"
                 ", perhaps a recursive function call?");
        return 0;
    }
    if (!ctx->d->flushed) {
        // oops, we are not ready for this!
        CL_ERROR("SymCallCache: cache entry found, but result not flushed yet"
                 ", perhaps a recursive function call?");
        return 0;
    }

    // all OK, return the cached ctx
    return ctx;
}

SymCallCtx& SymCallCache::getCallCtx(SymHeap                    heap,
                                     const CodeStorage::Fnc     &fnc,
                                     const CodeStorage::Insn    &insn)
{
    using namespace CodeStorage;

    // check insn validity
    const TOperandList &opList = insn.operands;
    CL_BREAK_IF(CL_INSN_CALL != insn.code || opList.size() < 2);

    // create SymProc and update the location info
    SymProc proc(heap, d->bt);
    proc.setLocation((d->lw = &insn.loc));
    d->heap = &heap;
    d->proc = &proc;

    // store Fnc ought to be called
    d->fnc = &fnc;
    CL_DEBUG_MSG(d->lw, "SymCallCache is looking for "
            << nameOf(fnc) << "()...");

    // check recursion depth (if any)
    const int uid = uidOf(fnc);
    d->nestLevel = d->bt->countOccurrencesOfFnc(uid);
    if (1 != d->nestLevel) {
        CL_WARN_MSG(d->lw, "support of call recursion is not stable yet");
        CL_NOTE_MSG(d->lw, "nestLevel is " << d->nestLevel);
    }

    // start with gl variables as the cut
    TCVarList cut(d->glVars);

    // initialize local variables of the called fnc
    d->createStackFrame(cut);
    d->setCallArgs(opList);
    d->proc->killInsn(insn);

    // prune heap
    SymHeap surround(heap.stor());
    splitHeapByCVars(&heap, cut, &surround);
    surround.valDestroyTarget(VAL_ADDR_OF_RET);
    
    // get either an existing ctx, or create a new one
    SymCallCtx *ctx = d->getCallCtx(uid, heap);
    if (!ctx)
        CL_TRAP;

    // not flushed yet
    ctx->d->flushed     = false;

    // keep some properties later required to process the results
    ctx->d->dst         = &opList[/* dst */ 0];
    ctx->d->nestLevel   = d->nestLevel;
    ctx->d->surround    = surround;
    return *ctx;
}
