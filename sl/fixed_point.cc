/*
 * Copyright (C) 2013 Kamil Dudka <kdudka@redhat.com>
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
#include "fixed_point.hh"

#include "cont_shape.hh"
#include "symtrace.hh"
#include "symutil.hh"
#include "worklist.hh"

#include <cl/cldebug.hh>
#include <cl/killer.hh>
#include <cl/storage.hh>

#include <boost/foreach.hpp>

namespace FixedPoint {

typedef const CodeStorage::Block                   *TBlock;

const THeapIdent InvalidHeap(-1, -1);

GenericInsn* GenericInsn::clone() const
{
    GenericInsn *insn = this->doClone();

    // this will stop if doClone() is not correctly overridden
    CL_BREAK_IF(typeid(*insn) != typeid(*this));

    return insn;
}

class ClInsn: public GenericInsn {
    public:
        ClInsn(TInsn insn):
            insn_(insn)
        {
        }

        virtual void writeToStream(std::ostream &str) const
        {
            str << *insn_;
        }

        virtual TInsn clInsn() const
        {
            return insn_;
        }

    private:
        TInsn insn_;

        virtual GenericInsn* doClone() const
        {
            return new ClInsn(*this);
        }
};

class TextInsn: public GenericInsn {
    public:
        TextInsn(const std::string &text):
            text_(text)
        {
        }

        virtual void writeToStream(std::ostream &str) const
        {
            str << text_;
        }

        virtual TInsn clInsn() const
        {
            return 0;
        }

    private:
        std::string text_;

        virtual GenericInsn *doClone() const
        {
            return new TextInsn(*this);
        }
};

const SymHeap *heapByIdent(const GlobalState &glState, const THeapIdent shIdent)
{
    const TLocIdx locIdx = shIdent.first;
    const LocalState &locState = glState[locIdx];

    const THeapIdx shIdx = shIdent.second;
    const SymHeap &sh = locState.heapList[shIdx];
    return &sh;
}

SymHeap *heapByIdent(GlobalState &glStateWr, const THeapIdent shIdent)
{
    const GlobalState &glState = const_cast<const GlobalState &>(glStateWr);
    return const_cast<SymHeap *>(heapByIdent(glState, shIdent));
}

const Shape *shapeByIdent(const GlobalState &glState, const TShapeIdent &shIdent)
{
    const THeapIdent heapIdent(shIdent.first);
    const LocalState &locState = glState[heapIdent.first];
    const TShapeList &shList = locState.shapeListByHeapIdx[heapIdent.second];
    return &shList[shIdent.second];
}

bool isTransparentInsn(const TInsn insn)
{
    const enum cl_insn_e code = insn->code;
    switch (code) {
        case CL_INSN_COND:
        case CL_INSN_JMP:
            return true;

        default:
            return false;
    }
}

typedef std::set<THeapIdent>                        THeapIdentSet;

class TraceIndex {
    public:
        TraceIndex(const GlobalState &glState):
            glState_(glState)
        {
        }

        void indexTraceOf(const TLocIdx);

        void nearestPredecessorOf(THeapIdentSet *pDst, const THeapIdent) const;

    private:
        typedef std::map<const Trace::Node *, THeapIdent> TLookup;
        const GlobalState          &glState_;
        TLookup                     lookup_;
};

void TraceIndex::indexTraceOf(const TLocIdx locIdx)
{
    const SymState &state = glState_[locIdx].heapList;
    const THeapIdx shCnt = state.size();
    for (THeapIdx shIdx = 0; shIdx < shCnt; ++shIdx) {
        const THeapIdent shIdent(locIdx, shIdx);
        const SymHeap &sh = state[shIdx];
        const Trace::Node *tr = sh.traceNode();

        // we should never change the target heap of an already indexed trace node
        CL_BREAK_IF(hasKey(lookup_, tr) && lookup_[tr] != shIdent);

        lookup_[tr] = shIdent;
    }
}

void TraceIndex::nearestPredecessorOf(
        THeapIdentSet              *pDst,
        const THeapIdent            shIdent)
    const
{
    const SymHeap *const sh = heapByIdent(glState_, shIdent);
    const Trace::Node *tr = sh->traceNode();

    WorkList<const Trace::Node *> wl(tr);
    while (wl.next(tr)) {
        // check the current trace node
        const TLookup::const_iterator it = lookup_.find(tr);
        if (it == lookup_.end()) {
            BOOST_FOREACH(const Trace::Node *trParent, tr->parents())
                wl.schedule(trParent);

            continue;
        }

        // found!
        const THeapIdent shPred = it->second;
        CL_BREAK_IF(heapByIdent(glState_, shPred)->traceNode() != tr);
        pDst->insert(shPred);
    }
}

typedef StateByInsn::TStateMap                      TStateMap;

typedef CleanList<LocalState>                       TStateList;
typedef CleanList<TraceEdge>                        TTraceList;
typedef std::map<TInsn, TLocIdx>                    TInsnLookup;

void loadHeaps(
        TStateList                 *pStateList,
        TInsnLookup                *pInsnLookup,
        const TFnc                  fnc,
        const TStateMap            &stateMap)
{
    typedef WorkList<TBlock> TWorkList;

    // traverse the original (block-oriented) control-flow graph
    TBlock bb = fnc->cfg.entry();
    TWorkList wl(bb);
    while (wl.next(bb)) {
        LocalState *locState;

        // go through instructions of the current basic block
        const TLocIdx insnCnt = bb->size();
        for (TLocIdx insnIdx = 0; insnIdx < insnCnt; ++insnIdx) {
            const TInsn insn = bb->operator[](insnIdx);

            BOOST_FOREACH(const TBlock bbNext, insn->targets)
                // schedule successor blocks for processing
                wl.schedule(bbNext);

            if (isTransparentInsn(insn))
                // skip instruction we do not want in the result
                continue;

            const TLocIdx locIdx = pStateList->size();
            if (insnIdx)
                // update successor location of the _previous_ instruction
                locState->cfgOutEdges.push_back(locIdx);

            // allocate a new location for the current instruction
            locState = new LocalState;
            locState->insn = new ClInsn(insn);
            pStateList->append(locState);

            // store the reverse mapping from instructions to locations
            (*pInsnLookup)[insn] = locIdx;

            // load heaps if a non-empty fixed-point is available for this loc
            const TStateMap::const_iterator it = stateMap.find(insn);
            if (it != stateMap.end()) {
                locState->heapList = it->second;
                Trace::waiveCloneOperation(locState->heapList);
            }

            // enlarge trace edges vectors
            const THeapIdx shCnt = locState->heapList.size();
            if (shCnt) {
                locState->traceInEdges.resize(shCnt);
                locState->traceOutEdges.resize(shCnt);
            }
        }
    }
}

void finalizeFlow(TStateList &stateList, const TInsnLookup &insnLookup)
{
    const TLocIdx locCnt = stateList.size();
    for (TLocIdx locIdx = 0; locIdx < locCnt; ++locIdx) {
        LocalState *locState = stateList[locIdx];
        const TInsn insn = locState->insn->clInsn();

        if (!locState->cfgOutEdges.empty()) {
            // non-terminal instructions are already handled in loadHeaps()
            CL_BREAK_IF(cl_is_term_insn(insn->code));
            continue;
        }

        // jump to terminal instruction (in most cases insn == term here)
        const TInsn term = insn->bb->back();
        CL_BREAK_IF(!cl_is_term_insn(term->code));

        BOOST_FOREACH(TBlock bb, term->targets) {
            TInsn dst = bb->front();

            // skip trivial basic blocks containing only single goto instruction
            while (1U == dst->targets.size()) {
                bb = dst->targets.front();
                dst = bb->front();
            }

            // create a new control-flow edge (originally block-level edge)
            const TLocIdx dstIdx = /* RO lookup */ insnLookup.find(dst)->second;
            locState->cfgOutEdges.push_back(dstIdx);
        }

        // tag loop-closing edges using the info provided by Code Listener
        BOOST_FOREACH(const unsigned tgIdx, term->loopClosingTargets)
            locState->cfgOutEdges[tgIdx].closesLoop = true;
    }

    // initialize backward control-flow edges
    for (TLocIdx srcIdx = 0; srcIdx < locCnt; ++srcIdx) {
        const LocalState *srcState = stateList[srcIdx];
        BOOST_FOREACH(CfgEdge oe, srcState->cfgOutEdges) {
            const TLocIdx dstIdx = oe.targetLoc;
            LocalState *dstState = stateList[dstIdx];
            oe.targetLoc = srcIdx;
            dstState->cfgInEdges.push_back(oe);
        }
    }
}

void initIdMapping(const GlobalState &glState, TraceEdge *te)
{
    const SymHeap *const shSrc = heapByIdent(glState, te->src);
    const SymHeap *const shDst = heapByIdent(glState, te->dst);

    const Trace::Node *const trSrc = shSrc->traceNode();
    const Trace::Node *const trDst = shDst->traceNode();

    Trace::resolveIdMapping(&te->objMap, trSrc, trDst);
}

void createTraceEdges(GlobalState &glState, TTraceList &traceList)
{
    const TLocIdx locCnt = glState.size();
    for (TLocIdx dstLocIdx = 0; dstLocIdx < locCnt; ++dstLocIdx) {
        LocalState &dstState = glState[dstLocIdx];

        // build trace index
        TraceIndex trIndex(glState);
        BOOST_FOREACH(const CfgEdge &ie, dstState.cfgInEdges)
            trIndex.indexTraceOf(ie.targetLoc);

        // try to find a predecessor for each local heap
        const THeapIdx heapCnt = dstState.heapList.size();
        for (THeapIdx dstHeapIdx = 0; dstHeapIdx < heapCnt; ++ dstHeapIdx) {
            const THeapIdent dstHeap(dstLocIdx, dstHeapIdx);

            THeapIdentSet heapSet;
            trIndex.nearestPredecessorOf(&heapSet, dstHeap);
            BOOST_FOREACH(const THeapIdent &srcHeap, heapSet) {
                // allocate a new trace edge
                TraceEdge *te = new TraceEdge(srcHeap, dstHeap);
                traceList.append(te);
                dstState.traceInEdges[dstHeapIdx].push_back(te);

                // store backward reference
                const TLocIdx srcLocIdx = srcHeap.first;
                LocalState &srcState = glState[srcLocIdx];
                srcState.traceOutEdges[srcHeap./* heap idx */second].push_back(te);

                // initialize object IDs mapping
                initIdMapping(glState, te);
            }
        }
    }
}

void detectContShapes(GlobalState &glState)
{
    const TLocIdx locCnt = glState.size();
    for (TLocIdx locIdx = 0; locIdx < locCnt; ++locIdx) {
        LocalState &locState = glState[locIdx];
        const SymState &state = locState.heapList;
        detectLocalContShapes(&locState.shapeListByHeapIdx, state);
    }
}

bool checkShapeMapping(
        const TObjSet              &srcObjs,
        const TObjSet              &dstObjs,
        const TObjectMapper        &objMap)
{
    // check whether objMap maps srcObjs _onto_ dstObjs
    TObjSet srcObjsImg;
    project<D_LEFT_TO_RIGHT>(objMap, &srcObjsImg, srcObjs);
    if (dstObjs != srcObjsImg)
        return false;

    // check whether objMap maps dstObjs _onto_ srcObjs
    TObjSet dstObjsImg;
    project<D_RIGHT_TO_LEFT>(objMap, &dstObjsImg, dstObjs);
    if (srcObjs != dstObjsImg)
        return false;

    // all OK!
    return true;
}

void detectShapeMappingCore(
        TraceEdge                  *te,
        const SymHeap              &shSrc,
        const SymHeap              &shDst,
        const TShapeList           &srcShapes,
        const TShapeList           &dstShapes)
{
    typedef std::map<TObjSet, TShapeIdx> TIndex;
    TIndex index;

    const TShapeIdx srcCnt = srcShapes.size();
    for (TShapeIdx srcIdx = 0; srcIdx < srcCnt; ++srcIdx) {
        TObjSet keySrc;
        objSetByShape(&keySrc, shSrc, srcShapes[srcIdx]);

        // translate the object IDs using the mapping stored in the edge
        TObjSet key;
        project<D_LEFT_TO_RIGHT>(te->objMap, &key, keySrc);

        // there should be no redefinitions
        CL_BREAK_IF(hasKey(index, key));

        index[key] = srcIdx;
    }

    const TShapeIdx dstCnt = dstShapes.size();
    for (TShapeIdx dstIdx = 0; dstIdx < dstCnt; ++dstIdx) {
        TObjSet keyDst;
        objSetByShape(&keyDst, shDst, dstShapes[dstIdx]);

        const TIndex::const_iterator it = index.find(keyDst);
        if (it == index.end())
            // not found
            continue;

        const TShapeIdx srcIdx = it->second;

        TObjSet keySrc;
        objSetByShape(&keySrc, shSrc, srcShapes[srcIdx]);
        if (!checkShapeMapping(keySrc, keyDst, te->objMap))
            // failed to check the mapping of shapes
            continue;

        te->csMap.insert(srcIdx, dstIdx);
    }
}

void detectShapeMapping(GlobalState &glState)
{
    const TLocIdx locCnt = glState.size();
    for (TLocIdx dstLocIdx = 0; dstLocIdx < locCnt; ++dstLocIdx) {
        const LocalState &dstState = glState[dstLocIdx];
        const THeapIdx shCnt = dstState.heapList.size();
        for (THeapIdx dstShIdx = 0; dstShIdx < shCnt; ++dstShIdx) {
            const TTraceEdgeList &tList = dstState.traceInEdges[dstShIdx];
            BOOST_FOREACH(TraceEdge *te, tList) {
                const TLocIdx srcLocIdx = te->src.first;
                const LocalState &srcState = glState[srcLocIdx];

                const THeapIdx srcShIdx = te->src.second;
                CL_BREAK_IF(dstShIdx != te->dst.second);

                const SymHeap &shSrc = srcState.heapList[srcShIdx];
                const SymHeap &shDst = dstState.heapList[dstShIdx];

                const TShapeList &srcShapes =
                    srcState.shapeListByHeapIdx[srcShIdx];
                const TShapeList &dstShapes =
                    dstState.shapeListByHeapIdx[dstShIdx];

                te->csMap.setNotFoundAction(TShapeMapper::NFA_RETURN_NOTHING);
                detectShapeMappingCore(te, shSrc, shDst, srcShapes, dstShapes);
            }
        }
    }
}

bool /* found */ detectSinglePrevShape(
        Shape                      *pShape,
        SymHeap                    &shDst,
        SymHeap                    &shSrc,
        TObjId                      objDst,
        TObjId                      objSrc)
{
    CL_BREAK_IF(1U != pShape->length);

    const BindingOff bOff = pShape->props.bOff;
    const TValId valNextSrc = valOfPtr(shSrc, objSrc, bOff.next);
    const TValId valPrevSrc = valOfPtr(shSrc, objSrc, bOff.prev);
    if (VAL_NULL != valNextSrc || VAL_NULL != valPrevSrc)
        // NULL terminator missing
        return false;

    const TSizeRange sizeDst = shDst.objSize(objDst);
    const TSizeRange sizeSrc = shSrc.objSize(objSrc);
    if (sizeDst != sizeSrc)
        // object size mismatch
        return false;

    const TObjType cltDst = shDst.objEstimatedType(objDst);
    const TObjType cltSrc = shSrc.objEstimatedType(objSrc);
    if (cltDst && cltSrc && (*cltDst != *cltSrc))
        // estimated type-info mismatch
        return false;

    // all OK
    pShape->entry = objSrc;
    return true;
}

bool /* found any */ detectPrevShapes(
        GlobalState                &glState,
        const TLocIdx               dstLocIdx,
        const THeapIdx              dstShIdx,
        const TShapeIdx             dstCsIdx)
{
    const LocalState &dstState = glState[dstLocIdx];
    const Shape &dstShape = dstState.shapeListByHeapIdx[dstShIdx][dstCsIdx];
    if (1U != dstShape.length)
        // only shapes consisting of exactly one object are supported for now
        return false;

    SymHeap &shDst = const_cast<SymHeap &>(dstState.heapList[dstShIdx]);
    const TObjId entry = dstShape.entry;
    TObjId obj = entry;

    const BindingOff bOff = dstShape.props.bOff;
    const TValId valNextDst = valOfPtr(shDst, obj, bOff.next);
    const TValId valPrevDst = valOfPtr(shDst, obj, bOff.prev);
    if (VAL_NULL != valNextDst || VAL_NULL != valPrevDst)
        // only shapes terminated by NULL are supported for now
        return false;

    bool foundAny = false;

    THeapIdent src(dstLocIdx, dstShIdx);
    for (;;) {
        LocalState &srcState = glState[src./* loc */first];
        const TTraceEdgeList &inEdges = srcState.traceInEdges[src.second];
        if (1U != inEdges.size())
            // only heaps with exactly one predecessor are supported for now
            break;

        const TraceEdge *te = inEdges.front();
        TObjectMapper::TVector mappedObjs;
        te->objMap.query<D_RIGHT_TO_LEFT>(&mappedObjs, obj);
        if (1U != mappedObjs.size())
            // only bijective object mapping is supported for now
            break;

        obj = mappedObjs.front();
        SymHeap *shSrc = heapByIdent(glState, te->src);
        if (!shSrc->isValid(obj))
            // the traced object no longer exists in the predecessor heap
            break;

        // jump to the predecessor
        src = te->src;
        LocalState &srcStatePrev = glState[src./* loc */first];

        Shape shape(dstShape);
        if (!detectSinglePrevShape(&shape, shDst, *shSrc, entry, obj))
            // not found in this step
            continue;

        TShapeList &shapeList = srcStatePrev.shapeListByHeapIdx[src.second];
        if (hasItem(shapeList, shape))
            // the shape has already been detected before
            continue;

        foundAny = true;
        shapeList.push_back(shape);
        CS_DEBUG("detectPrevShapes() appends a new container shape at loc #"
                << src./* loc */first);
    }

    return foundAny;
}

bool /* found any */ implyContShapesFromTrace(GlobalState &glState)
{
    bool foundAny = false;

    // for each location
    const TLocIdx locCnt = glState.size();
    for (TLocIdx dstLocIdx = 0; dstLocIdx < locCnt; ++dstLocIdx) {
        const LocalState &dstState = glState[dstLocIdx];

        // for each heap
        const THeapIdx shCnt = dstState.heapList.size();
        for (THeapIdx dstShIdx = 0; dstShIdx < shCnt; ++dstShIdx) {
            const TShapeList &shapes = dstState.shapeListByHeapIdx[dstShIdx];

            // for each container shape
            const TShapeIdx csCnt = shapes.size();
            for (TShapeIdx dstCsIdx = 0; dstCsIdx < csCnt; ++dstCsIdx) {
                TShapeMapper::TVector inbound;

                // for each ingoing trace edge
                const TTraceEdgeList &tList = dstState.traceInEdges[dstShIdx];
                BOOST_FOREACH(const TraceEdge *te, tList)
                    te->csMap.query<D_RIGHT_TO_LEFT>(&inbound, dstCsIdx);

                if (!inbound.empty())
                    // a predecessor already mapped
                    continue;

                if (detectPrevShapes(glState, dstLocIdx, dstShIdx, dstCsIdx))
                    foundAny = true;
            }
        }
    }

    return foundAny;
}

GlobalState* computeStateOf(const TFnc fnc, const TStateMap &stateByInsn)
{
    GlobalState *glState = new GlobalState;

    // build the skeleton (CFG nodes/edges, list of heaps per each node)
    TInsnLookup insnLookup;
    loadHeaps(&glState->stateList_, &insnLookup, fnc, stateByInsn);
    finalizeFlow(glState->stateList_, insnLookup);

    createTraceEdges(*glState, glState->traceList_);

    detectContShapes(*glState);

    detectShapeMapping(*glState);

    if (implyContShapesFromTrace(*glState))
        // new container shapes detected, chances are we will find new mapping
        detectShapeMapping(*glState);

    return glState;
}

void exportControlFlow(GlobalState *pDst, const GlobalState &glState)
{
    CL_BREAK_IF(!pDst->stateList_.empty());
    CL_BREAK_IF(!pDst->traceList_.empty());

    BOOST_FOREACH(const LocalState *locState, glState.stateList_) {
        LocalState *dupState = new LocalState;

        dupState->insn          = locState->insn->clone();
        dupState->cfgInEdges    = locState->cfgInEdges;
        dupState->cfgOutEdges   = locState->cfgOutEdges;

        pDst->stateList_.append(dupState);
    }
}

void StateRewriter::insertInsn(
        const TLocIdx               src,
        const TLocIdx               dst,
        const std::string          &insn)
{
    CL_NOTE("[ADT] inserting " << insn
            << " between locations #" << src << " -> #" << dst);

    // allocate a new instruction
    LocalState *locState = new LocalState;
    locState->insn = new TextInsn(insn);

    // append the instruction to the list
    const TLocIdx at = state_.size();
    state_.stateList_.append(locState);

    // resolve src/dst instructions
    LocalState &srcState = state_[src];
    LocalState &dstState = state_[dst];

    bool closesLoop = false;
    BOOST_FOREACH(CfgEdge &oe, srcState.cfgOutEdges) {
        if (dst != oe.targetLoc)
            continue;

        closesLoop = oe.closesLoop;
        oe.targetLoc = at;
        oe.closesLoop = false;
    }

    BOOST_FOREACH(CfgEdge &ie, dstState.cfgInEdges) {
        if (src != ie.targetLoc)
            continue;

        CL_BREAK_IF(closesLoop != ie.closesLoop);
        ie.targetLoc = at;
    }

    const CfgEdge ie(src);
    locState->cfgInEdges.push_back(ie);

    const CfgEdge oe(dst, closesLoop);
    locState->cfgOutEdges.push_back(oe);
}

void StateRewriter::replaceInsn(const TLocIdx at, const std::string &insn)
{
    CL_NOTE("[ADT] replacing insn #" << at << " by " << insn);
    LocalState &locState = state_[at];
    delete locState.insn;
    locState.insn = new TextInsn(insn);
}

void StateRewriter::dropInsn(const TLocIdx at)
{
    CL_NOTE("[ADT] removing insn #" << at);
    LocalState &locState = state_[at];
    delete locState.insn;
    locState.insn = 0;

    // iterate through all incoming edges
    BOOST_FOREACH(const CfgEdge &ie, locState.cfgInEdges) {
        CL_BREAK_IF(ie.closesLoop);
        LocalState &inState = state_[ie.targetLoc];
        TCfgEdgeList outEdges;

        BOOST_FOREACH(const CfgEdge &be, inState.cfgOutEdges) {
            if (at != be.targetLoc) {
                // keep unrelated CFG edges as they are
                outEdges.push_back(be);
                continue;
            }

            // redirect all edges previously going to 'at'
            BOOST_FOREACH(const CfgEdge &oe, locState.cfgOutEdges)
                outEdges.push_back(oe);
        }

        outEdges.swap(inState.cfgOutEdges);
    }

    // iterate through all outgoing edges
    BOOST_FOREACH(const CfgEdge &oe, locState.cfgOutEdges) {
        CL_BREAK_IF(oe.closesLoop);
        LocalState &outState = state_[oe.targetLoc];
        TCfgEdgeList inEdges;

        BOOST_FOREACH(const CfgEdge &be, outState.cfgInEdges) {
            if (at != be.targetLoc) {
                // keep unrelated CFG edges as they are
                inEdges.push_back(be);
                continue;
            }

            // redirect all edges previously coming from 'at'
            BOOST_FOREACH(const CfgEdge &ie, locState.cfgInEdges)
                inEdges.push_back(ie);
        }

        inEdges.swap(outState.cfgInEdges);
    }

    // finally detach 'at' from the graph completely
    locState.cfgInEdges.clear();
    locState.cfgOutEdges.clear();
}

bool StateRewriter::dedupOutgoingEdges(const TLocIdx at)
{
    LocalState &locState = state_[at];
    bool anyChange = false;

    // iterate through all outgoing edges
    BOOST_FOREACH(const CfgEdge &oe, locState.cfgOutEdges) {
        LocalState &outState = state_[oe.targetLoc];
        TCfgEdgeList inEdges;
        std::set<TLocIdx> inSet;

        BOOST_FOREACH(const CfgEdge &be, outState.cfgInEdges) {
            const TLocIdx dst = be.targetLoc;
            if (dst == at && !insertOnce(inSet, dst)) {
                // duplicate edge detected
                anyChange = true;
                continue;
            }

            // keep other CFG edges as they are
            inEdges.push_back(be);
        }

        inEdges.swap(outState.cfgInEdges);
    }

    if (!anyChange)
        // nothing changed actually
        return false;

    TCfgEdgeList outEdges;
    std::set<TLocIdx> outSet;

    // iterate through all outgoing edges
    BOOST_FOREACH(const CfgEdge &oe, locState.cfgOutEdges) {
        if (insertOnce(outSet, oe.targetLoc))
            outEdges.push_back(oe);
    }

    outEdges.swap(locState.cfgOutEdges);
    return true;
}

typedef CodeStorage::VarKiller::TVar                TVar;
typedef CodeStorage::VarKiller::TSet                TVarSet;
typedef std::vector<TVarSet>                        TVarSetByLoc;

void analyzeLiveVars(
        TVarSetByLoc               *pLive,
        TVarSetByLoc               *pKill,
        const GlobalState          &cfg)
{
    CL_BREAK_IF(!pLive->empty());
    CL_BREAK_IF(!pKill->empty());

    // allocate memory for the results
    const TLocIdx locCnt = cfg.size();
    pLive->resize(locCnt);
    pKill->resize(locCnt);

    // per-location data
    typedef CodeStorage::VarKiller::BlockData       TLocData;
    std::vector<TLocData> data(locCnt);

    // fixed-point computation scheduler
    typedef std::set<TLocIdx>                       TSched;
    TSched todo;

    // iterate through locations
    for (TLocIdx locIdx = 0; locIdx < locCnt; ++locIdx) {
        const LocalState &locNode = cfg[locIdx];
        TLocData &locData = data[locIdx];
        TVarSet &killSet = locData.kill;

        if (!locNode.insn)
            // an already removed instruction
            continue;

        const TInsn insn = locNode.insn->clInsn();
        if (insn) {
            // compute gen/kill sets for regular CL instruction
            scanInsn(&locData, insn);

            if (1U < locNode.cfgOutEdges.size()) {
                // assume branch instruction
                CL_BREAK_IF(1U != killSet.size());
                locData.gen.insert(*killSet.begin());
            }
        }
        else {
            CL_WARN("removeDeadCode() ignores " << *locNode.insn);
        }

        // write killed variables as the set is already final
        (*pKill)[locIdx] = killSet;

        // schedule this node for processing
        todo.insert(locIdx);
    }

    // compute the fixed-point
    while (!todo.empty()) {
        // pick any from the set of scheduled locations
        TSched::iterator it = todo.begin();
        const TLocIdx locIdx = *it;
        todo.erase(it);

        TLocData &locData = data[locIdx];
        bool anyChange = false;

        // go through all variables generated by successors
        const LocalState &locNode = cfg[locIdx];
        BOOST_FOREACH(CfgEdge e, locNode.cfgOutEdges) {
            TLocData &srcData = data[e.targetLoc];
            BOOST_FOREACH(TVar var, srcData.gen) {
                if (hasKey(srcData.kill, var))
                    // the variable is killed by the next insn, do not propagate
                    continue;

                // update 'gen' set of the current location
                if (insertOnce(locData.gen, var))
                    anyChange = true;
            }
        }

        if (!anyChange)
            // nothing updated actually
            continue;

        // schedule all predecessors
        BOOST_FOREACH(CfgEdge e, locNode.cfgInEdges)
            todo.insert(e.targetLoc);
    }

    // finally write the resulting 'gen' sets
    for (TLocIdx locIdx = 0; locIdx < locCnt; ++locIdx)
        (*pLive)[locIdx] = data[locIdx].gen;
}

bool removeRedundantBranching(GlobalState *pState)
{
    StateRewriter writer(pState);
    bool anyChange = false;

    const TLocIdx locCnt = pState->size();
    for (TLocIdx locIdx = 0; locIdx < locCnt; ++locIdx) {
        if (!writer.dedupOutgoingEdges(locIdx))
            continue;

        // XXX: assume redundant branch condition with no side effect
        writer.dropInsn(locIdx);
        anyChange = true;
    }

    return anyChange;
}

void removeDeadCode(GlobalState *pState)
{
    do {
        // compute sets live and killed vars per location
        TVarSetByLoc liveVarsPerLoc;
        TVarSetByLoc killVarsPerLoc;
        analyzeLiveVars(&liveVarsPerLoc, &killVarsPerLoc, *pState);

        StateRewriter writer(pState);

        // iterate through locations
        const TLocIdx locCnt = pState->size();
        for (TLocIdx locIdx = 0; locIdx < locCnt; ++locIdx) {
            const LocalState &locState = (*pState)[locIdx];
            if (!locState.insn)
                // there is no instruction to be removed
                continue;

            const TInsn insn = locState.insn->clInsn();
            if (!insn)
                // we remove only Code Listener instructions for now
                continue;

            const TVarSet &killSet = killVarsPerLoc[locIdx];
            const unsigned cntKill = killSet.size();
            if (!cntKill)
                // there is no variable being written
                continue;

            CL_BREAK_IF(1U < cntKill);
            const CodeStorage::VarKiller::TVar var = *killSet.begin();
            if (hasKey(liveVarsPerLoc[locIdx], var))
                // var alive
                continue;

            const enum cl_insn_e code = insn->code;
            switch (code) {
                case CL_INSN_UNOP:
                case CL_INSN_BINOP:
                    break;

                default:
                    CL_WARN("removeDeadCode() refuses to remove: " << *insn);
                    continue;
            }

            // remove the current location from the CFG
            writer.dropInsn(locIdx);
        }
    }
    while (removeRedundantBranching(pState));
}

void sl_dump(const TShapeMapper &m)
{
    std::cout << "TShapeMapper: ";
    m.prettyPrint(std::cout);
    std::cout << "\n";
}

void sl_dump(const TObjectMapper &m)
{
    std::cout << "TObjectMapper: ";
    m.prettyPrint(std::cout);
    std::cout << "\n";
}

} // namespace FixedPoint
