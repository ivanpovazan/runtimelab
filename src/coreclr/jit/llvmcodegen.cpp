// ================================================================================================================
// |                                            LLVM-based codegen                                                |
// ================================================================================================================

#include "llvm.h"

//------------------------------------------------------------------------
// Compile: Compile IR to LLVM, adding to the LLVM Module
//
void Llvm::Compile()
{
    const char* mangledName = GetMangledMethodName(_info.compMethodHnd);
    _function = _module->getFunction(mangledName);
    _debugFunction = nullptr;
    _debugMetadata.diCompileUnit = nullptr;

    if (_function == nullptr)
    {
        _function = Function::Create(getFunctionType(), Function::ExternalLinkage, 0U, mangledName,
                                     _module); // TODO: ExternalLinkage forced as linked from old module
    }

    // mono does this via Javascript (pal_random.js), but prefer not to introduce that dependency as it limits the ability to run out of the browser.
    // Copy the temporary workaround from the IL->LLVM generator for now.
    if (!strcmp(mangledName, "S_P_CoreLib_Interop__GetRandomBytes"))
    {
        // this would normally fill the buffer parameter, but we'll just leave the buffer as is and that will be our "random" data for now
        llvm::BasicBlock* llvmBlock = llvm::BasicBlock::Create(_llvmContext, "", _function);
        _builder.SetInsertPoint(llvmBlock);
        _builder.CreateRetVoid();
        return;
    }

    // TODO-LLVM: enable. Currently broken because RyuJit inserts RPI helpers for RPI methods, then we
    // also create an RPI wrapper stub, resulting in a double transition.
    if (_compiler->opts.IsReversePInvoke())
    {
        failFunctionCompilation();
    }

    if (_compiler->opts.compDbgInfo)
    {
        const char* documentFileName = GetDocumentFileName();
        if (documentFileName && *documentFileName != '\0')
        {
            _debugMetadata = getOrCreateDebugMetadata(documentFileName);
        }
    }

    generateProlog();

    class LlvmCompileDomTreeVisitor : public DomTreeVisitor<LlvmCompileDomTreeVisitor>
    {
        Llvm* m_llvm;

    public:
        LlvmCompileDomTreeVisitor(Compiler* compiler, Llvm* llvm)
            : DomTreeVisitor(compiler, compiler->fgSsaDomTree)
            , m_llvm(llvm)
        {
        }

        void PreOrderVisit(BasicBlock * block) const
        {
            // TODO-LLVM: finret basic blocks
            if (block->bbJumpKind == BBJ_EHFINALLYRET)
            {
                m_llvm->failFunctionCompilation();
            }

            m_llvm->startImportingBasicBlock(block);

            llvm::BasicBlock* entry = m_llvm->getLLVMBasicBlockForBlock(block);
            m_llvm->_builder.SetInsertPoint(entry);
            for (GenTree* node : LIR::AsRange(block))
            {
                m_llvm->startImportingNode();
                m_llvm->visitNode(node);
            }
            m_llvm->endImportingBasicBlock(block);
        }
    };

    LlvmCompileDomTreeVisitor visitor(_compiler, this);
    visitor.WalkTree();

    fillPhis();

    if (_debugFunction != nullptr)
    {
        _diBuilder->finalizeSubprogram(_debugFunction);
    }

#if DEBUG
    JITDUMP("\n===================================================================================================================\n");
    JITDUMP("LLVM IR for %s after codegen:\n", _compiler->info.compFullName);
    JITDUMP("-------------------------------------------------------------------------------------------------------------------\n\n");
    JITDUMPEXEC(_function->dump());

    if (llvm::verifyFunction(*_function, &llvm::errs()))
    {
        printf("function failed %s\n", mangledName);
    }
#endif
}

void Llvm::generateProlog()
{
    JITDUMP("\n=============== Generating prolog:\n");

    llvm::BasicBlock* prologBlock = llvm::BasicBlock::Create(_llvmContext, "Prolog", _function);
    _prologBuilder.SetInsertPoint(prologBlock);

    initializeLocals();

    llvm::BasicBlock* block0 = getLLVMBasicBlockForBlock(_compiler->fgFirstBB);
    _prologBuilder.SetInsertPoint(_prologBuilder.CreateBr(block0));
    _builder.SetInsertPoint(block0);
}

void Llvm::initializeLocals()
{
    m_allocas = std::vector<Value*>(_compiler->lvaCount, nullptr);

    for (unsigned lclNum = 0; lclNum < _compiler->lvaCount; lclNum++)
    {
        LclVarDsc* varDsc = _compiler->lvaGetDesc(lclNum);

        if (varDsc->lvRefCnt() == 0)
        {
            continue;
        }

        // Needed because of "implicitly referenced" locals.
        if (!canStoreLocalOnLlvmStack(varDsc))
        {
            continue;
        }

        // See "genCheckUseBlockInit", "fgInterBlockLocalVarLiveness" and "SsaBuilder::RenameVariables" as references
        // for the zero-init logic.
        //
        Type* lclLlvmType = getLlvmTypeForLclVar(varDsc);
        Value* initValue = nullptr;
        Value* zeroValue = llvm::Constant::getNullValue(lclLlvmType);
        if (varDsc->lvIsParam)
        {
            assert(varDsc->lvLlvmArgNum != BAD_LLVM_ARG_NUM);
            initValue = _function->getArg(varDsc->lvLlvmArgNum);
        }
        else
        {
            // If the local is in SSA, things are somewhat simple: we must provide an initial value if there is an
            // "implicit" def, and must not if there is not.
            if (_compiler->lvaInSsa(lclNum))
            {
                // Needed because of "implicitly referenced" locals.
                if (varDsc->lvPerSsaData.GetCount() == 0)
                {
                    continue;
                }

                bool hasImplicitDef = varDsc->GetPerSsaData(SsaConfig::FIRST_SSA_NUM)->GetAssignment() == nullptr;
                if (!hasImplicitDef)
                {
                    // Nothing else needs to be done for this local.
                    assert(!varDsc->lvMustInit);
                    continue;
                }

                // SSA locals are always tracked; use liveness' determination on whether we need to zero-init.
                if (varDsc->lvMustInit)
                {
                    initValue = zeroValue;
                }
            }
            else if (!varDsc->lvHasExplicitInit) // We do not need to zero-init locals with explicit inits.
            {
                // This reduces to, essentially, "!isTemp && compInitMem", the general test for whether
                // we need to zero-initialize, under the assumption there are use-before-def references.
                if (!_compiler->fgVarNeedsExplicitZeroInit(lclNum, /* bbInALoop */ false, /* bbIsReturn */ false))
                {
                    // For untracked locals, we have to be conservative. For tracked ones, we can query the
                    // "lvMustInit" bit liveness has set.
                    if (!varDsc->lvTracked || varDsc->lvMustInit)
                    {
                        initValue = zeroValue;
                    }
                }
            }

            JITDUMP("Setting V%02u's initial value to %s\n", lclNum, (initValue == zeroValue) ? "zero" : "uninit");
        }

        // Reset the bit so that subsequent dumping reflects our decision here.
        varDsc->lvMustInit = initValue == zeroValue;

        // If we're not zero-initializing, use a frozen undef value. This will ensure we don't run
        // into UB issues with undefined values (which uninitialized allocas produce, see LangRef)
        if (initValue == nullptr)
        {
            initValue = llvm::UndefValue::get(lclLlvmType);
            initValue = _prologBuilder.CreateFreeze(initValue);
            JITDUMPEXEC(initValue->dump());
        }

        assert(initValue->getType() == lclLlvmType);

        if (isLlvmFrameLocal(varDsc))
        {
            Instruction* allocaInst = _prologBuilder.CreateAlloca(lclLlvmType);
            m_allocas[lclNum] = allocaInst;
            JITDUMPEXEC(allocaInst->dump());

            Instruction* storeInst = _prologBuilder.CreateStore(initValue, allocaInst);
            JITDUMPEXEC(storeInst->dump());
        }
        else
        {
            assert(_compiler->lvaInSsa(lclNum));
            _localsMap.Set({lclNum, SsaConfig::FIRST_SSA_NUM}, initValue);
        }
    }
}

void Llvm::startImportingBasicBlock(BasicBlock* block)
{
    JITDUMP("\n=============== Generating ");
    JITDUMPEXEC(block->dspBlockHeader(_compiler, /* showKind */ true, /* showFlags */ true));

    _currentBlock = block;
}

void Llvm::endImportingBasicBlock(BasicBlock* block)
{
    if ((block->bbJumpKind == BBJ_NONE) && block->bbNext != nullptr)
    {
        _builder.CreateBr(getLLVMBasicBlockForBlock(block->bbNext));
        return;
    }
    if ((block->bbJumpKind == BBJ_ALWAYS) && block->bbJumpDest != nullptr)
    {
        _builder.CreateBr(getLLVMBasicBlockForBlock(block->bbJumpDest));
        return;
    }
    if ((block->bbJumpKind == BBJ_THROW))
    {
        _builder.CreateUnreachable();
        return;
    }
    //TODO: other jump kinds
}

void Llvm::fillPhis()
{
    for (PhiPair phiPair : _phiPairs)
    {
        llvm::PHINode* llvmPhiNode = phiPair.llvmPhiNode;

        for (GenTreePhi::Use& use : phiPair.irPhiNode->Uses())
        {
            GenTreePhiArg* phiArg = use.GetNode()->AsPhiArg();
            unsigned       lclNum = phiArg->GetLclNum();
            unsigned       ssaNum = phiArg->GetSsaNum();

            Value* localPhiArg = _localsMap[{lclNum, ssaNum}];
            Value* phiRealArgValue;
            Instruction* castRequired = getCast(localPhiArg, llvmPhiNode->getType());
            if (castRequired != nullptr)
            {
                // This cast is needed when
                // 1) The phi arg real type is short and the definition is the actual longer type, e.g. for bool/int
                // 2) There is a pointer difference, e.g. i8* v i32* and perhaps different levels of indirection: i8** and i8*
                llvm::BasicBlock::iterator phiInsertPoint = _builder.GetInsertPoint();
                llvm::BasicBlock* phiBlock = _builder.GetInsertBlock();
                Instruction* predBlockTerminator = getLLVMBasicBlockForBlock(phiArg->gtPredBB)->getTerminator();

                _builder.SetInsertPoint(predBlockTerminator);
                phiRealArgValue = _builder.Insert(castRequired);
                _builder.SetInsertPoint(phiBlock, phiInsertPoint);
            }
            else
            {
                phiRealArgValue = localPhiArg;
            }
            llvmPhiNode->addIncoming(phiRealArgValue, getLLVMBasicBlockForBlock(phiArg->gtPredBB));
        }
    }
}

Value* Llvm::getGenTreeValue(GenTree* op)
{
    return _sdsuMap[op];
}

//------------------------------------------------------------------------
// consumeValue: Get the Value* "node" produces when consumed as "targetLlvmType".
//
// During codegen, we follow the "normalize on demand" convention, i. e.
// the IR nodes produce "raw" values that have exactly the types of nodes,
// preserving small types, pointers, etc. However, the user in the IR
// consumes "actual" types, and this is the method where we normalize
// to those types. We could have followed the reverse convention and
// normalized on production of "Value*"s, but we presume the "on demand"
// convention is more efficient LLVM-IR-size-wise. It allows us to avoid
// situations where we'd be upcasting only to immediately truncate, which
// would be the case for small typed arguments and relops feeding jumps,
// to name a few examples.
//
// Arguments:
//    node           - the node for which to obtain the normalized value of
//    targetLlvmType - the LLVM type through which the user uses "node"
//
// Return Value:
//    The normalized value, of "targetLlvmType" type. If the latter wasn't
//    provided, the raw value is returned, except for small types, which
//    are still extended to INT.
//
Value* Llvm::consumeValue(GenTree* node, Type* targetLlvmType)
{
    Value* nodeValue = getGenTreeValue(node);
    Value* finalValue = nodeValue;

    if (targetLlvmType == nullptr)
    {
        if (!nodeValue->getType()->isIntegerTy())
        {
            return finalValue;
        }

        targetLlvmType = getLlvmTypeForVarType(genActualType(node));
    }

    if (nodeValue->getType() != targetLlvmType)
    {
        // int to pointer type (TODO-LLVM: WASM64: use POINTER_BITs when set correctly, also below for getInt32Ty)
        if (nodeValue->getType() == Type::getInt32Ty(_llvmContext) && targetLlvmType->isPointerTy())
        {
            return _builder.CreateIntToPtr(nodeValue, targetLlvmType);
        }

        // pointer to ints
        if (nodeValue->getType()->isPointerTy() && targetLlvmType == Type::getInt32Ty(_llvmContext))
        {
            return _builder.CreatePtrToInt(nodeValue, Type::getInt32Ty(_llvmContext));
        }

        // i32* e.g symbols, to i8*
        if (nodeValue->getType()->isPointerTy() && targetLlvmType->isPointerTy())
        {
            return _builder.CreateBitCast(nodeValue, targetLlvmType);
        }

        // int and smaller int conversions
        assert(targetLlvmType->isIntegerTy() && nodeValue->getType()->isIntegerTy() &&
               nodeValue->getType()->getPrimitiveSizeInBits() <= 32 && targetLlvmType->getPrimitiveSizeInBits() <= 32);
        if (nodeValue->getType()->getPrimitiveSizeInBits() < targetLlvmType->getPrimitiveSizeInBits())
        {
            var_types trueNodeType = TYP_UNDEF;

            switch (node->OperGet())
            {
                case GT_CALL:
                    trueNodeType = static_cast<var_types>(node->AsCall()->gtReturnType);
                    break;

                case GT_LCL_VAR:
                    trueNodeType = _compiler->lvaGetDesc(node->AsLclVarCommon())->TypeGet();
                    break;

                case GT_EQ:
                case GT_NE:
                case GT_LT:
                case GT_LE:
                case GT_GE:
                case GT_GT:
                    // This is the special case for relops. Ordinary codegen "just knows" they need zero-extension.
                    assert(nodeValue->getType() == Type::getInt1Ty(_llvmContext));
                    trueNodeType = TYP_UBYTE;
                    break;

                case GT_CAST:
                    trueNodeType = node->AsCast()->CastToType();
                    break;
                default:
                    trueNodeType = node->TypeGet();
                    break;
            }

            assert(varTypeIsSmall(trueNodeType));

            finalValue = varTypeIsSigned(trueNodeType) ? _builder.CreateSExt(nodeValue, targetLlvmType)
                : _builder.CreateZExt(nodeValue, targetLlvmType);
        }
        else
        {
            // Truncate.
            finalValue = _builder.CreateTrunc(nodeValue, targetLlvmType);
        }
    }

    return finalValue;
}

void Llvm::mapGenTreeToValue(GenTree* node, Value* nodeValue)
{
    _sdsuMap.Set(node, nodeValue);
}

void Llvm::startImportingNode()
{
    if (_debugMetadata.diCompileUnit != nullptr && _currentOffsetDiLocation == nullptr)
    {
        unsigned int lineNo = GetOffsetLineNumber(_currentOffset.GetLocation().GetOffset());

        _currentOffsetDiLocation = createDebugFunctionAndDiLocation(_debugMetadata, lineNo);
        _builder.SetCurrentDebugLocation(_currentOffsetDiLocation);
    }
}

void Llvm::visitNode(GenTree* node)
{
    JITDUMPEXEC(_compiler->gtDispLIRNode(node, "Generating: "));
    INDEBUG(auto lastInstrIter = --_builder.GetInsertPoint());

    switch (node->OperGet())
    {
        case GT_ADD:
            buildAdd(node->AsOp());
            break;
        case GT_DIV:
            buildDiv(node);
            break;
        case GT_CALL:
            buildCall(node);
            break;
        case GT_CAST:
            buildCast(node->AsCast());
            break;
        case GT_LCLHEAP:
            buildLclHeap(node->AsUnOp());
            break;
        case GT_CNS_DBL:
            buildCnsDouble(node->AsDblCon());
            break;
        case GT_CNS_INT:
            buildCnsInt(node);
            break;
        case GT_CNS_LNG:
            buildCnsLng(node);
            break;
        case GT_IL_OFFSET:
            _currentOffset = node->AsILOffset()->gtStmtDI;
            _currentOffsetDiLocation = nullptr;
            break;
        case GT_IND:
            buildInd(node->AsIndir());
            break;
        case GT_JTRUE:
            buildJTrue(node, getGenTreeValue(node->AsOp()->gtOp1));
            break;
        case GT_LCL_FLD:
            buildLocalField(node->AsLclFld());
            break;
        case GT_LCL_VAR:
            buildLocalVar(node->AsLclVar());
            break;
        case GT_LCL_VAR_ADDR:
        case GT_LCL_FLD_ADDR:
            buildLocalVarAddr(node->AsLclVarCommon());
            break;
        case GT_LSH:
        case GT_RSH:
        case GT_RSZ:
            buildShift(node->AsOp());
            break;
        case GT_EQ:
        case GT_NE:
        case GT_LE:
        case GT_LT:
        case GT_GE:
        case GT_GT:
            buildCmp(node->AsOp());
            break;
        case GT_NEG:
        case GT_NOT:
            buildUnaryOperation(node);
            break;
        case GT_NO_OP:
            emitDoNothingCall();
            break;
        case GT_NULLCHECK:
            buildNullCheck(node->AsIndir());
            break;
        case GT_OBJ:
        case GT_BLK:
            buildBlk(node->AsBlk());
            break;
        case GT_PHI:
            buildEmptyPhi(node->AsPhi());
            break;
        case GT_PHI_ARG:
        case GT_PUTARG_TYPE:
            break;
        case GT_RETURN:
            buildReturn(node);
            break;
        case GT_STORE_LCL_VAR:
            buildStoreLocalVar(node->AsLclVar());
            break;
        case GT_STOREIND:
            buildStoreInd(node->AsStoreInd());
            break;
        case GT_STORE_BLK:
        case GT_STORE_OBJ:
            buildStoreBlk(node->AsBlk());
            break;
        case GT_AND:
        case GT_OR:
        case GT_XOR:
            buildBinaryOperation(node);
            break;
        case GT_FIELD_LIST:
        case GT_INIT_VAL:
            // These ('contained') nodes aways generate code as part of their parent.
            break;
        default:
            failFunctionCompilation();
    }

#ifdef DEBUG
    // Dump all instructions that contributed to the code generated by this node.
    //
    if (_compiler->verbose)
    {
        for (auto instrIter = ++lastInstrIter; instrIter != _builder.GetInsertPoint(); ++instrIter)
        {
            instrIter->dump();
        }
    }
#endif // DEBUG
}

void Llvm::buildLocalVar(GenTreeLclVar* lclVar)
{
    Value*       llvmRef;
    unsigned int lclNum = lclVar->GetLclNum();
    unsigned int ssaNum = lclVar->GetSsaNum();
    LclVarDsc*   varDsc = _compiler->lvaGetDesc(lclVar);

    if (isLlvmFrameLocal(varDsc))
    {
        llvmRef = _builder.CreateLoad(m_allocas[lclNum]);
    }
    else
    {
        llvmRef = _localsMap[{lclNum, ssaNum}];
    }

    // Implicit truncating from long to int.
    if ((varDsc->TypeGet() == TYP_LONG) && lclVar->TypeIs(TYP_INT))
    {
        llvmRef = _builder.CreateTrunc(llvmRef, Type::getInt32Ty(_llvmContext));
    }

    mapGenTreeToValue(lclVar, llvmRef);
}

void Llvm::buildStoreLocalVar(GenTreeLclVar* lclVar)
{
    unsigned lclNum = lclVar->GetLclNum();
    LclVarDsc* varDsc = _compiler->lvaGetDesc(lclVar);
    Type* destLlvmType = getLlvmTypeForLclVar(varDsc);
    Value* localValue = nullptr;

    // zero initialization check
    if (lclVar->TypeIs(TYP_STRUCT) && lclVar->gtGetOp1()->IsIntegralConst(0))
    {
        localValue = llvm::Constant::getNullValue(destLlvmType);
    }
    else
    {
        localValue = consumeValue(lclVar->gtGetOp1(), destLlvmType);
    }

    if (isLlvmFrameLocal(varDsc))
    {
        _builder.CreateStore(localValue, m_allocas[lclNum]);
    }
    else
    {
        _localsMap.Set({lclNum, lclVar->GetSsaNum()}, localValue);
    }
}

// in case we haven't seen the phi args yet, create just the phi nodes and fill in the args at the end
void Llvm::buildEmptyPhi(GenTreePhi* phi)
{
    LclVarDsc* varDsc = _compiler->lvaGetDesc(phi->Uses().begin()->GetNode()->AsPhiArg());
    Type* lclLlvmType = getLlvmTypeForLclVar(varDsc);

    llvm::PHINode* llvmPhiNode = _builder.CreatePHI(lclLlvmType, 2);
    _phiPairs.push_back({ phi, llvmPhiNode });

    mapGenTreeToValue(phi, llvmPhiNode);
}

void Llvm::buildLocalField(GenTreeLclFld* lclFld)
{
    assert(!lclFld->TypeIs(TYP_STRUCT));

    unsigned   lclNum = lclFld->GetLclNum();
    LclVarDsc* varDsc = _compiler->lvaGetDesc(lclNum);
    assert(isLlvmFrameLocal(varDsc));

    // TODO-LLVM: if this is an only value type field, or at offset 0, we can optimize.
    Value* structAddrValue = m_allocas[lclNum];
    Value* structAddrInt8Ptr = castIfNecessary(structAddrValue, Type::getInt8PtrTy(_llvmContext));
    Value* fieldAddressValue = gepOrAddr(structAddrInt8Ptr, lclFld->GetLclOffs());
    Value* fieldAddressTypedValue =
        castIfNecessary(fieldAddressValue, getLlvmTypeForVarType(lclFld->TypeGet())->getPointerTo());

    mapGenTreeToValue(lclFld, _builder.CreateLoad(fieldAddressTypedValue));
}

void Llvm::buildLocalVarAddr(GenTreeLclVarCommon* lclAddr)
{
    unsigned int lclNum = lclAddr->GetLclNum();
    if (lclAddr->isLclField())
    {
        Value* bytePtr = castIfNecessary(m_allocas[lclNum], Type::getInt8PtrTy(_llvmContext));
        mapGenTreeToValue(lclAddr, gepOrAddr(bytePtr, lclAddr->GetLclOffs()));
    }
    else
    {
        mapGenTreeToValue(lclAddr, m_allocas[lclNum]);
    }
}

void Llvm::buildAdd(GenTreeOp* node)
{
    Value* op1Value = consumeValue(node->gtGetOp1());
    Value* op2Value = consumeValue(node->gtGetOp2());
    Type* op1Type = op1Value->getType();
    Type* op2Type = op2Value->getType();

    Value* addValue;
    if (op1Type->isPointerTy() && op2Type->isIntegerTy())
    {
        // GEPs scale indices, bitcasting to i8* makes them equivalent to the raw offsets we have in IR
        addValue = _builder.CreateGEP(castIfNecessary(op1Value, Type::getInt8PtrTy(_llvmContext)), op2Value);
    }
    else if (op1Type->isIntegerTy() && (op1Type == op2Type))
    {
        addValue = _builder.CreateAdd(op1Value, op2Value);
    }
    else
    {
        // unsupported add type combination
        failFunctionCompilation();
    }

    mapGenTreeToValue(node, addValue);
}

void Llvm::buildDiv(GenTree* node)
{
    Type* targetType = getLlvmTypeForVarType(node->TypeGet());
    Value* dividendValue = consumeValue(node->gtGetOp1(), targetType);
    Value* divisorValue  = consumeValue(node->gtGetOp2(), targetType);
    Value* resultValue   = nullptr;
    // TODO-LLVM: exception handling.  Div by 0 and INT32/64_MIN / -1
    switch (node->TypeGet())
    {
        case TYP_FLOAT:
        case TYP_DOUBLE:
            resultValue = _builder.CreateFDiv(dividendValue, divisorValue);
            break;

        default:
            resultValue = _builder.CreateSDiv(dividendValue, divisorValue);
            break;
    }

    mapGenTreeToValue(node, resultValue);
}

void Llvm::buildCast(GenTreeCast* cast)
{
    var_types castFromType = genActualType(cast->CastOp());
    var_types castToType = cast->CastToType();
    Value* castFromValue = consumeValue(cast->CastOp(), getLlvmTypeForVarType(castFromType));
    Value* castValue = nullptr;
    Type* castToLlvmType = getLlvmTypeForVarType(castToType);

    // TODO-LLVM: handle checked ("gtOverflow") casts.
    switch (castFromType)
    {
        case TYP_INT:
        case TYP_LONG:
            switch (castToType)
            {
                case TYP_BOOL:
                case TYP_BYTE:
                case TYP_UBYTE:
                case TYP_SHORT:
                case TYP_USHORT:
                case TYP_INT:
                case TYP_UINT:
                    // "Cast(integer -> small type)" is "s/zext<int>(truncate<small type>)".
                    // Here we will truncate and leave the extension for the user to consume.
                    castValue = _builder.CreateTrunc(castFromValue, castToLlvmType);
                    break;

                case TYP_LONG:
                    castValue = cast->IsUnsigned()
                        ? _builder.CreateZExt(castFromValue, castToLlvmType)
                        : _builder.CreateSExt(castFromValue, castToLlvmType);
                    break;

                case TYP_FLOAT:
                case TYP_DOUBLE:
                    castValue = cast->IsUnsigned()
                        ? _builder.CreateUIToFP(castFromValue, castToLlvmType)
                        : _builder.CreateSIToFP(castFromValue, castToLlvmType);
                    break;

                default:
                    failFunctionCompilation(); // NYI
            }
            break;

        case TYP_FLOAT:
        case TYP_DOUBLE:
            switch (castToType)
            {
                case TYP_FLOAT:
                case TYP_DOUBLE:
                    castValue = _builder.CreateFPCast(castFromValue, castToLlvmType);
                    break;
                case TYP_BYTE:
                case TYP_SHORT:
                case TYP_INT:
                case TYP_LONG:
                    castValue = _builder.CreateFPToSI(castFromValue, castToLlvmType);
                    break;

                case TYP_BOOL:
                case TYP_UBYTE:
                case TYP_USHORT:
                case TYP_UINT:
                case TYP_ULONG:
                    castValue = _builder.CreateFPToUI(castFromValue, castToLlvmType);
                    break;

                default:
                    unreached();
            }
            break;

        default:
            failFunctionCompilation(); // NYI
    }

    mapGenTreeToValue(cast, castValue);
}

void Llvm::buildLclHeap(GenTreeUnOp* lclHeap)
{
    GenTree* sizeNode = lclHeap->gtGetOp1();
    assert(genActualTypeIsIntOrI(sizeNode));

    Value* sizeValue = consumeValue(sizeNode, getLlvmTypeForVarType(genActualType(sizeNode)));
    Value* lclHeapValue;

    // A zero-sized LCLHEAP yields a null pointer.
    if (sizeNode->IsIntegralConst(0))
    {
        lclHeapValue = llvm::Constant::getNullValue(Type::getInt8PtrTy(_llvmContext));
    }
    else
    {
        llvm::AllocaInst* allocaInst = _builder.CreateAlloca(Type::getInt8Ty(_llvmContext), sizeValue);

        // LCLHEAP (aka IL's "localloc") is specified to return a pointer "...aligned so that any built-in data type
        // can be stored there using the stind instructions", so we'll be a bit conservative and align it maximally.
        llvm::Align allocaAlignment = llvm::Align(genTypeSize(TYP_DOUBLE));
        allocaInst->setAlignment(allocaAlignment);

        // "If the localsinit flag on the method is true, the block of memory returned is initialized to 0".
        if (_compiler->info.compInitMem)
        {
            _builder.CreateMemSet(allocaInst, _builder.getInt8(0), sizeValue, allocaAlignment);
        }

        if (!sizeNode->IsIntegralConst()) // Build: %lclHeapValue = (%sizeValue != 0) ? "alloca" : "null".
        {
            Value* zeroSizeValue = llvm::Constant::getNullValue(sizeValue->getType());
            Value* isSizeNotZeroValue = _builder.CreateCmp(llvm::CmpInst::ICMP_NE, sizeValue, zeroSizeValue);
            Value* nullValue = llvm::Constant::getNullValue(Type::getInt8PtrTy(_llvmContext));

            lclHeapValue = _builder.CreateSelect(isSizeNotZeroValue, allocaInst, nullValue);
        }
        else
        {
            lclHeapValue = allocaInst;
        }
    }

    mapGenTreeToValue(lclHeap, lclHeapValue);
}

void Llvm::buildCmp(GenTreeOp* node)
{
    using Predicate = llvm::CmpInst::Predicate;

    bool isIntOrPtr = varTypeIsIntegralOrI(node->gtGetOp1());
    bool isUnsigned = node->IsUnsigned();
    bool isUnordered = (node->gtFlags & GTF_RELOP_NAN_UN) != 0;
    Predicate predicate;
    switch (node->OperGet())
    {
        case GT_EQ:
            predicate = isIntOrPtr ? Predicate::ICMP_EQ : (isUnordered ? Predicate::FCMP_UEQ : Predicate::FCMP_OEQ);
            break;
        case GT_NE:
            predicate = isIntOrPtr ? Predicate::ICMP_NE : (isUnordered ? Predicate::FCMP_UNE : Predicate::FCMP_ONE);
            break;
        case GT_LE:
            predicate = isIntOrPtr ? (isUnsigned ? Predicate::ICMP_ULE : Predicate::ICMP_SLE)
                                   : (isUnordered ? Predicate::FCMP_ULE : Predicate::FCMP_OLE);
            break;
        case GT_LT:
            predicate = isIntOrPtr ? (isUnsigned ? Predicate::ICMP_ULT : Predicate::ICMP_SLT)
                                   : (isUnordered ? Predicate::FCMP_ULT : Predicate::FCMP_OLT);
            break;
        case GT_GE:
            predicate = isIntOrPtr ? (isUnsigned ? Predicate::ICMP_UGE : Predicate::ICMP_SGE)
                                   : (isUnordered ? Predicate::FCMP_UGE : Predicate::FCMP_OGE);
            break;
        case GT_GT:
            predicate = isIntOrPtr ? (isUnsigned ? Predicate::ICMP_UGT : Predicate::ICMP_SGT)
                                   : (isUnordered ? Predicate::FCMP_UGT : Predicate::FCMP_OGT);
            break;
        default:
            unreached();
    }

    // Comparing refs and ints is valid LIR, but not LLVM so handle that case by converting the int to a ref.
    Value* op1Value = consumeValue(node->gtGetOp1());
    Value* op2Value = consumeValue(node->gtGetOp2());
    Type* op1Type = op1Value->getType();
    Type* op2Type = op2Value->getType();
    if (op1Type != op2Type)
    {
        assert((op1Type->isPointerTy() && op2Type->isIntegerTy()) ||
               (op1Type->isIntegerTy() && op2Type->isPointerTy()));
        if (op1Type->isPointerTy())
        {
            op2Value = _builder.CreateIntToPtr(op2Value, op1Type);
        }
        else
        {
            op1Value = _builder.CreateIntToPtr(op1Value, op2Type);
        }
    }

    mapGenTreeToValue(node, _builder.CreateCmp(predicate, op1Value, op2Value));
}

void Llvm::buildCnsDouble(GenTreeDblCon* node)
{
    if (node->TypeIs(TYP_DOUBLE))
    {
        mapGenTreeToValue(node, llvm::ConstantFP::get(Type::getDoubleTy(_llvmContext), node->gtDconVal));
    }
    else
    {
        assert(node->TypeIs(TYP_FLOAT));
        mapGenTreeToValue(node, llvm::ConstantFP::get(Type::getFloatTy(_llvmContext), node->gtDconVal));
    }
}

void Llvm::buildCnsInt(GenTree* node)
{
    if (node->gtType == TYP_INT)
    {
        if (node->IsIconHandle())
        {
            // TODO-LLVM : consider lowering these to "IND(CLS_VAR_ADDR)"
            if (node->IsIconHandle(GTF_ICON_TOKEN_HDL) || node->IsIconHandle(GTF_ICON_CLASS_HDL) ||
                node->IsIconHandle(GTF_ICON_METHOD_HDL) || node->IsIconHandle(GTF_ICON_FIELD_HDL))
            {
                const char* symbolName = GetMangledSymbolName((void*)(node->AsIntCon()->IconValue()));
                AddCodeReloc((void*)node->AsIntCon()->IconValue());
                mapGenTreeToValue(node, _builder.CreateLoad(getOrCreateExternalSymbol(symbolName)));
            }
            else
            {
                //TODO-LLVML: other ICON handle types
                failFunctionCompilation();
            }
        }
        else
        {
            mapGenTreeToValue(node, _builder.getInt32(node->AsIntCon()->IconValue()));
        }
        return;
    }
    if (node->gtType == TYP_REF)
    {
        ssize_t intCon = node->AsIntCon()->gtIconVal;
        if (node->IsIconHandle(GTF_ICON_STR_HDL))
        {
            const char* symbolName = GetMangledSymbolName((void *)(node->AsIntCon()->IconValue()));
            AddCodeReloc((void*)node->AsIntCon()->IconValue());
            mapGenTreeToValue(node, _builder.CreateLoad(getOrCreateExternalSymbol(symbolName)));
            return;
        }
        // TODO: delete this check, just handling string constants and null ptr stores for now, other TYP_REFs not implemented yet
        if (intCon != 0)
        {
            failFunctionCompilation();
        }

        mapGenTreeToValue(node, _builder.CreateIntToPtr(_builder.getInt32(intCon), Type::getInt8PtrTy(_llvmContext))); // TODO: wasm64
        return;
    }
    failFunctionCompilation();
}

void Llvm::buildCnsLng(GenTree* node)
{
    mapGenTreeToValue(node, _builder.getInt64(node->AsLngCon()->LngValue()));
}

void Llvm::buildCall(GenTree* node)
{
    GenTreeCall* call = node->AsCall();
    if (call->gtCallType == CT_HELPER)
    {
        buildHelperFuncCall(call);
    }
    else if ((call->gtCallType == CT_USER_FUNC || call->gtCallType == CT_INDIRECT) &&
             !call->IsVirtualStub() /* TODO: Virtual stub not implemented */)
    {
        buildUserFuncCall(call);
    }
    else
    {
        failFunctionCompilation();
    }
}

void Llvm::buildHelperFuncCall(GenTreeCall* call)
{
    if (call->gtCallMethHnd == _compiler->eeFindHelper(CORINFO_HELP_READYTORUN_GENERIC_HANDLE) ||
        call->gtCallMethHnd == _compiler->eeFindHelper(CORINFO_HELP_READYTORUN_GENERIC_STATIC_BASE) ||
        call->gtCallMethHnd == _compiler->eeFindHelper(CORINFO_HELP_GVMLOOKUP_FOR_SLOT) || /* generates an extra parameter in the signature */
        call->gtCallMethHnd == _compiler->eeFindHelper(CORINFO_HELP_TYPEHANDLE_TO_RUNTIMETYPE) || /* misses an arg in the signature somewhere, not the shadow stack */
        call->gtCallMethHnd == _compiler->eeFindHelper(CORINFO_HELP_READYTORUN_DELEGATE_CTOR) ||
        call->gtCallMethHnd == _compiler->eeFindHelper(CORINFO_HELP_THROW_PLATFORM_NOT_SUPPORTED)) // TODO-LLVM: we are not generating an unreachable after this call
    {
        // TODO-LLVM
        failFunctionCompilation();
    }

    if (call->gtCallMethHnd == _compiler->eeFindHelper(CORINFO_HELP_READYTORUN_STATIC_BASE))
    {
        const char* symbolName = GetMangledSymbolName(CORINFO_METHOD_HANDLE(call->gtEntryPoint.handle));
        Function* llvmFunc = _module->getFunction(symbolName);
        if (llvmFunc == nullptr)
        {
            llvmFunc = Function::Create(buildHelperLlvmFunctionType(call, true), Function::ExternalLinkage, 0U, symbolName, _module); // TODO: ExternalLinkage forced as defined in ILC module
        }

        // Replacement for _info.compCompHnd->recordRelocation(nullptr, gtCall->gtEntryPoint.handle, IMAGE_REL_BASED_REL32);
        AddCodeReloc(call->gtEntryPoint.handle);

        mapGenTreeToValue(call, _builder.CreateCall(llvmFunc, getShadowStackForCallee()));
        return;
    }
    else
    {
        fgArgInfo* argInfo = call->fgArgInfo;
        unsigned int argCount = argInfo->ArgCount();
        fgArgTabEntry** argTable = argInfo->ArgTable();
        std::vector<OperandArgNum> sortedArgs = std::vector<OperandArgNum>(argCount);
        OperandArgNum* sortedData = sortedArgs.data();
        bool requiresShadowStack = helperRequiresShadowStack(call->gtCallMethHnd);

        //TODO-LLVM: refactor calling code with user calls.
        for (unsigned i = 0; i < argCount; i++)
        {
            fgArgTabEntry* curArgTabEntry = argTable[i];
            unsigned int   argNum = curArgTabEntry->argNum;
            OperandArgNum  opAndArg = { argNum, curArgTabEntry->GetNode() };
            sortedData[argNum] = opAndArg;
        }

        void* pAddr = nullptr;

        CorInfoHelpFunc helperNum = _compiler->eeGetHelperNum(call->gtCallMethHnd);
        void* addr = _compiler->compGetHelperFtn(helperNum, &pAddr);
        const char* symbolName = GetMangledSymbolName(addr);
        Function* llvmFunc = _module->getFunction(symbolName);
        if (llvmFunc == nullptr)
        {
            llvmFunc = Function::Create(buildHelperLlvmFunctionType(call, requiresShadowStack), Function::ExternalLinkage, 0U, symbolName, _module);
        }

        AddCodeReloc(addr);

        std::vector<llvm::Value*> argVec;
        unsigned argIx = 0;

        Value* shadowStackForCallee = getShadowStackForCallee();
        if (requiresShadowStack)
        {
            argVec.push_back(shadowStackForCallee);
            argIx++;
        }
        else
        {
            // we may come back into managed from the unmanaged call so store the shadowstack
            _builder.CreateStore(shadowStackForCallee, getOrCreateExternalSymbol("t_pShadowStackTop", Type::getInt8PtrTy(_llvmContext)));
        }

        for (OperandArgNum opAndArg : sortedArgs)
        {
            if ((opAndArg.operand->gtOper == GT_CNS_INT) && opAndArg.operand->IsIconHandle())
            {
                void* iconValue = (void*)(opAndArg.operand->AsIntCon()->IconValue());
                const char* methodTableName = GetMangledSymbolName(iconValue);
                AddCodeReloc(iconValue);
                argVec.push_back(castIfNecessary(_builder.CreateLoad(castIfNecessary(getOrCreateExternalSymbol(methodTableName), Type::getInt32PtrTy(_llvmContext)->getPointerTo())), llvmFunc->getArg(argIx)->getType()));
            }
            else
            {
                argVec.push_back(consumeValue(opAndArg.operand, llvmFunc->getArg(argIx)->getType()));
            }
            argIx++;
        }
        // TODO-LLVM: If the block has a handler, this will need to be an invoke.  E.g. create a CallOrInvoke as per ILToLLVMImporter
        mapGenTreeToValue(call, _builder.CreateCall(llvmFunc, llvm::ArrayRef<Value*>(argVec)));
    }
}

void Llvm::buildUserFuncCall(GenTreeCall* call)
{
    llvm::FunctionCallee llvmFuncCallee;

    if (call->gtCallType == CT_USER_FUNC || call->gtCallType == CT_INDIRECT)
    {
        if (call->IsVirtualVtable() || call->gtCallType == CT_INDIRECT)
        {
            FunctionType* functionType = createFunctionTypeForCall(call);
            GenTree* calleeNode = call->IsVirtualVtable() ? call->gtControlExpr : call->gtCallAddr;

            Value* funcPtr = castIfNecessary(getGenTreeValue(calleeNode), functionType->getPointerTo());

            llvmFuncCallee = {functionType, funcPtr};
        }
        else
        {
            const char* symbolName = GetMangledSymbolName(call->gtEntryPoint.handle);

            AddCodeReloc(call->gtEntryPoint.handle);
            Function* llvmFunc = getOrCreateLlvmFunction(symbolName, call);

            llvmFuncCallee = llvmFunc;
        }
    }

    std::vector<llvm::Value*> argVec = std::vector<llvm::Value*>();

    GenTreePutArgType* lastArg = nullptr;
    for (GenTreeCall::Use& use : call->Args())
    {
        lastArg = use.GetNode()->AsPutArgType();

        GenTree* argNode     = lastArg->gtGetOp1();
        Type*    argLlvmType = getLlvmTypeForCorInfoType(lastArg->GetCorInfoType(), lastArg->GetClsHnd());
        Value*   argValue;

        if (argNode->OperIs(GT_FIELD_LIST))
        {
            assert(lastArg->GetCorInfoType() == CORINFO_TYPE_VALUECLASS);
            argValue = buildFieldList(argNode->AsFieldList(), argLlvmType);
        }
        else
        {
            argValue = consumeValue(argNode, argLlvmType);
        }

        argVec.push_back(argValue);
    }

    Value* llvmCall = _builder.CreateCall(llvmFuncCallee, ArrayRef<Value*>(argVec));
    mapGenTreeToValue(call, llvmCall);
}

Value* Llvm::buildFieldList(GenTreeFieldList* fieldList, Type* llvmType)
{
    assert(fieldList->TypeIs(TYP_STRUCT));

    if (llvmType->isStructTy())
    {
        Value* alloca = _builder.CreateAlloca(llvmType);
        Value* allocaAsBytePtr = _builder.CreatePointerCast(alloca, Type::getInt8PtrTy(_llvmContext));

        for (GenTreeFieldList::Use& use : fieldList->Uses())
        {
            Value* fieldAddr = gepOrAddr(allocaAsBytePtr, use.GetOffset());
            Type*  fieldType = getLlvmTypeForVarType(use.GetType());
            fieldAddr        = castIfNecessary(fieldAddr, fieldType->getPointerTo());
            _builder.CreateStore(consumeValue(use.GetNode(), fieldType), fieldAddr);
        }

        return _builder.CreateLoad(alloca);
    }

    // single primitive type wrapped in struct
    assert(fieldList->Uses().begin()->GetNext() == nullptr);

    return consumeValue(fieldList->Uses().begin()->GetNode(), llvmType);
}

void Llvm::buildInd(GenTreeIndir* indNode)
{
    Type* loadLlvmType = getLlvmTypeForVarType(indNode->TypeGet());
    Value* addrValue = consumeValue(indNode->Addr(), loadLlvmType->getPointerTo());

    emitNullCheckForIndir(indNode, addrValue);
    Value* loadValue = _builder.CreateLoad(loadLlvmType, addrValue);

    mapGenTreeToValue(indNode, loadValue);
}

void Llvm::buildBlk(GenTreeBlk* blkNode)
{
    Type* blkLlvmType = getLlvmTypeForStruct(blkNode->GetLayout());
    Value* addrValue = consumeValue(blkNode->Addr(), blkLlvmType->getPointerTo());

    emitNullCheckForIndir(blkNode, addrValue);
    Value* blkValue = _builder.CreateLoad(blkLlvmType, addrValue);

    mapGenTreeToValue(blkNode, blkValue);
}

void Llvm::buildStoreInd(GenTreeStoreInd* storeIndOp)
{
    GCInfo::WriteBarrierForm wbf = getGCInfo()->gcIsWriteBarrierCandidate(storeIndOp, storeIndOp->Data());

    Type* storeLlvmType = getLlvmTypeForVarType(storeIndOp->TypeGet());
    Type* addrLlvmType = (wbf == GCInfo::WBF_NoBarrier) ? storeLlvmType->getPointerTo() : Type::getInt8PtrTy(_llvmContext);
    Value* addrValue = consumeValue(storeIndOp->Addr(), addrLlvmType);;
    Value* dataValue = consumeValue(storeIndOp->Data(), storeLlvmType);

    emitNullCheckForIndir(storeIndOp, addrValue);

    switch (wbf)
    {
        case GCInfo::WBF_BarrierUnchecked:
            _builder.CreateCall(getOrCreateRhpAssignRef(), {addrValue, dataValue});
            break;

        case GCInfo::WBF_BarrierChecked:
        case GCInfo::WBF_BarrierUnknown:
            _builder.CreateCall(getOrCreateRhpCheckedAssignRef(), {addrValue, dataValue});
            break;

        case GCInfo::WBF_NoBarrier:
            _builder.CreateStore(dataValue, addrValue);
            break;

        default:
            unreached();
    }
}

void Llvm::buildStoreBlk(GenTreeBlk* blockOp)
{
    ClassLayout* layout = blockOp->GetLayout();
    GenTree* addrNode = blockOp->Addr();
    GenTree* dataNode = blockOp->Data();
    Value* addrValue = consumeValue(addrNode, Type::getInt8PtrTy(_llvmContext));

    emitNullCheckForIndir(blockOp, addrValue);

    // Check for the "initblk" operation ("dataNode" is either INIT_VAL or constant zero).
    if (blockOp->OperIsInitBlkOp())
    {
        Value* fillValue = dataNode->OperIsInitVal() ? consumeValue(dataNode->gtGetOp1(), Type::getInt8Ty(_llvmContext))
                                                     : _builder.getInt8(0);
        _builder.CreateMemSet(addrValue, fillValue, _builder.getInt32(layout->GetSize()), llvm::Align());
        return;
    }

    Value* dataValue = consumeValue(dataNode, getLlvmTypeForStruct(layout));
    if (layout->HasGCPtr() && ((blockOp->gtFlags & GTF_IND_TGT_NOT_HEAP) == 0) && !addrNode->OperIsLocalAddr())
    {
        storeObjAtAddress(addrValue, dataValue, getStructDesc(layout->GetClassHandle()));
    }
    else
    {
        _builder.CreateStore(dataValue, castIfNecessary(addrValue, dataValue->getType()->getPointerTo()));
    }
}

void Llvm::buildUnaryOperation(GenTree* node)
{
    Value* result;
    Value* op1Value = consumeValue(node->gtGetOp1(), getLlvmTypeForVarType(node->TypeGet()));

    switch (node->OperGet())
    {
        case GT_NEG:
            if (op1Value->getType()->isFloatingPointTy())
            {
                result = _builder.CreateFNeg(op1Value, "fneg");
            }
            else
            {
                result = _builder.CreateNeg(op1Value, "neg");
            }
            break;
        case GT_NOT:
            result = _builder.CreateNot(op1Value, "not");
            break;
        default:
            failFunctionCompilation();  // TODO-LLVM: other binary operators
    }
    mapGenTreeToValue(node, result);
}

void Llvm::buildBinaryOperation(GenTree* node)
{
    Value* result;
    Type*  targetType = getLlvmTypeForVarType(node->TypeGet());
    Value* op1 = consumeValue(node->gtGetOp1(), targetType);
    Value* op2 = consumeValue(node->gtGetOp2(), targetType);

    switch (node->OperGet())
    {
        case GT_AND:
            result = _builder.CreateAnd(op1, op2, "and");
            break;
        case GT_OR:
            result = _builder.CreateOr(op1, op2, "or");
            break;
        case GT_XOR:
            result = _builder.CreateXor(op1, op2, "xor");
            break;
        default:
            failFunctionCompilation();  // TODO-LLVM: other binary operaions
    }
    mapGenTreeToValue(node, result);
}

void Llvm::buildShift(GenTreeOp* node)
{
    Type*  llvmTargetType = getLlvmTypeForVarType(node->TypeGet());
    Value* numBitsToShift = consumeValue(node->gtOp2, getLlvmTypeForVarType(node->gtOp2->TypeGet()));

    // LLVM requires the operands be the same type as the shift itself.
    // Shift counts are assumed to never be negative, so we zero extend.
    if (numBitsToShift->getType()->getPrimitiveSizeInBits() < llvmTargetType->getPrimitiveSizeInBits())
    {
        numBitsToShift = _builder.CreateZExt(numBitsToShift, llvmTargetType);
    }

    Value* op1Value = consumeValue(node->gtOp1, llvmTargetType);
    Value* result;

    switch (node->OperGet())
    {
        case GT_LSH:
            result = _builder.CreateShl(op1Value, numBitsToShift, "lsh");
            break;
        case GT_RSH:
            result = _builder.CreateAShr(op1Value, numBitsToShift, "rsh");
            break;
        case GT_RSZ:
            result = _builder.CreateLShr(op1Value, numBitsToShift, "rsz");
            break;
        default:
            failFunctionCompilation();  // TODO-LLVM: other shift types
    }
    mapGenTreeToValue(node, result);
}

void Llvm::buildReturn(GenTree* node)
{
    if (node->TypeIs(TYP_VOID))
    {
        _builder.CreateRetVoid();
        return;
    }

    GenTree* retValNode = node->gtGetOp1();
    Type* retLlvmType = getLlvmTypeForCorInfoType(_sigInfo.retType, _sigInfo.retTypeClass);
    Value* retValValue;
    // Special-case returning zero-initialized structs.
    if (node->TypeIs(TYP_STRUCT) && retValNode->IsIntegralConst(0))
    {
        retValValue = llvm::Constant::getNullValue(retLlvmType);
    }
    else if (genActualType(node) != genActualType(retValNode))
    {
        // TODO-LLVM: remove these cases in lowering.
        failFunctionCompilation();
    }
    else
    {
        retValValue = consumeValue(retValNode, retLlvmType);
    }

    _builder.CreateRet(retValValue);
}

void Llvm::buildJTrue(GenTree* node, Value* opValue)
{
    _builder.CreateCondBr(opValue, getLLVMBasicBlockForBlock(_currentBlock->bbJumpDest), getLLVMBasicBlockForBlock(_currentBlock->bbNext));
}

void Llvm::buildNullCheck(GenTreeIndir* nullCheckNode)
{
    Value* addrValue = consumeValue(nullCheckNode->Addr(), Type::getInt8PtrTy(_llvmContext));
    emitNullCheckForIndir(nullCheckNode, addrValue);
}

void Llvm::storeObjAtAddress(Value* baseAddress, Value* data, StructDesc* structDesc)
{
    unsigned fieldCount = structDesc->getFieldCount();
    unsigned bytesStored = 0;

    for (unsigned i = 0; i < fieldCount; i++)
    {
        FieldDesc* fieldDesc = structDesc->getFieldDesc(i);
        unsigned   fieldOffset = fieldDesc->getFieldOffset();
        Value*     address     = gepOrAddr(baseAddress, fieldOffset);

        if (structDesc->hasSignificantPadding() && fieldOffset > bytesStored)
        {
            bytesStored += buildMemCpy(baseAddress, bytesStored, fieldOffset, address);
        }

        Value* fieldData = nullptr;
        if (data->getType()->isStructTy())
        {
            const llvm::StructLayout* structLayout = _module->getDataLayout().getStructLayout(static_cast<llvm::StructType*>(data->getType()));

            unsigned llvmFieldIndex = structLayout->getElementContainingOffset(fieldOffset);
            fieldData               = _builder.CreateExtractValue(data, llvmFieldIndex);
        }
        else
        {
            // single field IL structs are not LLVM structs
            fieldData = data;
        }

        if (fieldData->getType()->isStructTy())
        {
            assert(fieldDesc->getClassHandle() != NO_CLASS_HANDLE);

            // recurse into struct
            storeObjAtAddress(address, fieldData, getStructDesc(fieldDesc->getClassHandle()));

            bytesStored += fieldData->getType()->getPrimitiveSizeInBits() / BITS_PER_BYTE;
        }
        else
        {
            if (fieldDesc->isGcPointer())
            {
                // we can't be sure the address is on the heap, it could be the result of pointer arithmetic on a local var
                _builder.CreateCall(getOrCreateRhpCheckedAssignRef(),
                                    ArrayRef<Value*>{address,
                                    castIfNecessary(fieldData, Type::getInt8PtrTy(_llvmContext))});
                bytesStored += TARGET_POINTER_SIZE;
            }
            else
            {
                _builder.CreateStore(fieldData, castIfNecessary(address, fieldData->getType()->getPointerTo()));

                bytesStored += fieldData->getType()->getPrimitiveSizeInBits() / BITS_PER_BYTE;
            }
        }
    }

    unsigned llvmStructSize = data->getType()->getPrimitiveSizeInBits() / BITS_PER_BYTE;
    if (structDesc->hasSignificantPadding() && llvmStructSize > bytesStored)
    {
        Value* srcAddress = _builder.CreateGEP(baseAddress, _builder.getInt32(bytesStored));

        buildMemCpy(baseAddress, bytesStored, llvmStructSize, srcAddress);
    }
}

// Copies endOffset - startOffset bytes, endOffset is exclusive.
unsigned Llvm::buildMemCpy(Value* baseAddress, unsigned startOffset, unsigned endOffset, Value* srcAddress)
{
    Value* destAddress = gepOrAddr(baseAddress, startOffset);
    unsigned size = endOffset - startOffset;

    _builder.CreateMemCpy(destAddress, llvm::Align(), srcAddress, llvm::Align(), size);

    return size;
}

void Llvm::emitDoNothingCall()
{
    if (_doNothingFunction == nullptr)
    {
        _doNothingFunction = Function::Create(FunctionType::get(Type::getVoidTy(_llvmContext), ArrayRef<Type*>(), false), Function::ExternalLinkage, 0U, "llvm.donothing", _module);
    }
    _builder.CreateCall(_doNothingFunction);
}

void Llvm::emitNullCheckForIndir(GenTreeIndir* indir, Value* addrValue)
{
    if ((indir->gtFlags & GTF_IND_NONFAULTING) == 0)
    {
        Function* throwIfNullFunc = getOrCreateThrowIfNullFunction();
        addrValue = castIfNecessary(addrValue, Type::getInt8PtrTy(_llvmContext));

        // TODO-LLVM: this shadow stack passing is not efficient.
        buildLlvmCallOrInvoke(throwIfNullFunc, {getShadowStackForCallee(), addrValue});
    }
}

void Llvm::buildThrowException(llvm::IRBuilder<>& builder, const char* helperClass, const char* helperMethodName, Value* shadowStack)
{
    CORINFO_METHOD_HANDLE methodHandle = GetCompilerHelpersMethodHandle(helperClass, helperMethodName);
    const char* mangledName = GetMangledMethodName(methodHandle);

    Function* llvmFunc = _module->getFunction(mangledName);

    if (llvmFunc == nullptr)
    {
        // assume ExternalLinkage, if the function is defined in the clrjit module, then it is replaced and an
        // extern added to the Ilc module
        llvmFunc = Function::Create(FunctionType::get(Type::getVoidTy(_llvmContext), {Type::getInt8PtrTy(_llvmContext)},
                                                      false),
                                    Function::ExternalLinkage, 0U, mangledName, _module);
       AddCodeReloc(methodHandle);
    }

    builder.CreateCall(llvmFunc, {shadowStack});
    builder.CreateUnreachable();
}

void Llvm::buildLlvmCallOrInvoke(Function* callee, ArrayRef<Value*> args)
{
    // TODO-LLVM: invoke if callsite has exception handler
    _builder.CreateCall(callee, args);
}

FunctionType* Llvm::getFunctionType()
{
    // TODO-LLVM: delete this when these signatures implemented
    if (_sigInfo.hasExplicitThis() || _sigInfo.hasTypeArg())
        failFunctionCompilation();

    std::vector<llvm::Type*> argVec(_llvmArgCount);
    llvm::Type*              retLlvmType;

    for (unsigned i = 0; i < _compiler->lvaCount; i++)
    {
        LclVarDsc* varDsc = _compiler->lvaGetDesc(i);
        if (varDsc->lvIsParam)
        {
            assert(varDsc->lvLlvmArgNum != BAD_LLVM_ARG_NUM);
            argVec[varDsc->lvLlvmArgNum] = getLlvmTypeForLclVar(varDsc);
        }
    }

    retLlvmType = _retAddressLclNum == BAD_VAR_NUM
        ? getLlvmTypeForCorInfoType(_sigInfo.retType, _sigInfo.retTypeClass)
        : Type::getVoidTy(_llvmContext);

    return FunctionType::get(retLlvmType, ArrayRef<Type*>(argVec), false);
}

Function* Llvm::getOrCreateLlvmFunction(const char* symbolName, GenTreeCall* call)
{
    Function* llvmFunc = _module->getFunction(symbolName);

    if (llvmFunc == nullptr)
    {
        // assume ExternalLinkage, if the function is defined in the clrjit module, then it is replaced and an
        // extern added to the Ilc module
        llvmFunc =
            Function::Create(createFunctionTypeForCall(call), Function::ExternalLinkage, 0U, symbolName, _module);
    }
    return llvmFunc;
}

FunctionType* Llvm::createFunctionTypeForCall(GenTreeCall* call)
{
    llvm::Type* retLlvmType = getLlvmTypeForCorInfoType(call->gtCorInfoType, call->gtRetClsHnd);

    std::vector<llvm::Type*> argVec = std::vector<llvm::Type*>();

    for (GenTreeCall::Use& use : call->Args())
    {
        GenTreePutArgType* putArg = use.GetNode()->AsPutArgType();
        argVec.push_back(getLlvmTypeForCorInfoType(putArg->GetCorInfoType(), putArg->GetClsHnd()));
    }

    return FunctionType::get(retLlvmType, ArrayRef<Type*>(argVec), false);
}

FunctionType* Llvm::buildHelperLlvmFunctionType(GenTreeCall* call, bool withShadowStack)
{
    Type* retLlvmType = getLlvmTypeForVarType(call->TypeGet());
    std::vector<llvm::Type*> argVec;

    if (withShadowStack)
    {
        argVec.push_back(Type::getInt8PtrTy(_llvmContext));
    }

    for (GenTreeCall::Use& use : call->Args())
    {
        Type* argLlvmType = getLlvmTypeForVarType(use.GetNode()->TypeGet());
        argVec.push_back(argLlvmType);
    }

    return FunctionType::get(retLlvmType, ArrayRef<llvm::Type*>(argVec), false);
}

bool Llvm::helperRequiresShadowStack(CORINFO_METHOD_HANDLE corinfoMethodHnd)
{
    //TODO-LLVM: is there a better way to identify managed helpers?
    //Probably want to lower the math helpers to ordinary GT_CASTs and
    //handle in the LLVM (as does ILToLLVMImporter) to avoid this overhead
    return corinfoMethodHnd == _compiler->eeFindHelper(CORINFO_HELP_TYPEHANDLE_TO_RUNTIMETYPEHANDLE) ||
        corinfoMethodHnd == _compiler->eeFindHelper(CORINFO_HELP_GVMLOOKUP_FOR_SLOT) ||
        corinfoMethodHnd == _compiler->eeFindHelper(CORINFO_HELP_DBL2INT_OVF) ||
        corinfoMethodHnd == _compiler->eeFindHelper(CORINFO_HELP_DBL2LNG_OVF) ||
        corinfoMethodHnd == _compiler->eeFindHelper(CORINFO_HELP_DBL2UINT_OVF) ||
        corinfoMethodHnd == _compiler->eeFindHelper(CORINFO_HELP_DBL2ULNG_OVF) ||
        corinfoMethodHnd == _compiler->eeFindHelper(CORINFO_HELP_LMOD) ||
        corinfoMethodHnd == _compiler->eeFindHelper(CORINFO_HELP_LDIV) ||
        corinfoMethodHnd == _compiler->eeFindHelper(CORINFO_HELP_LMUL_OVF) ||
        corinfoMethodHnd == _compiler->eeFindHelper(CORINFO_HELP_ULMUL_OVF) ||
        corinfoMethodHnd == _compiler->eeFindHelper(CORINFO_HELP_ULDIV) ||
        corinfoMethodHnd == _compiler->eeFindHelper(CORINFO_HELP_ULMOD) ||
        corinfoMethodHnd == _compiler->eeFindHelper(CORINFO_HELP_OVERFLOW) ||
        corinfoMethodHnd == _compiler->eeFindHelper(CORINFO_HELP_TYPEHANDLE_TO_RUNTIMETYPE) ||
        corinfoMethodHnd == _compiler->eeFindHelper(CORINFO_HELP_THROW_PLATFORM_NOT_SUPPORTED);
}

Value* Llvm::getOrCreateExternalSymbol(const char* symbolName, Type* symbolType)
{
    if (symbolType == nullptr)
    {
        symbolType = Type::getInt32PtrTy(_llvmContext);
    }

    Value* symbol = _module->getGlobalVariable(symbolName);
    if (symbol == nullptr)
    {
        symbol = new llvm::GlobalVariable(*_module, symbolType, false, llvm::GlobalValue::LinkageTypes::ExternalLinkage, (llvm::Constant*)nullptr, symbolName);
    }
    return symbol;
}

Function* Llvm::getOrCreateRhpAssignRef()
{
    Function* llvmFunc = _module->getFunction("RhpAssignRef");
    if (llvmFunc == nullptr)
    {
        llvmFunc = Function::Create(FunctionType::get(Type::getVoidTy(_llvmContext),
                                                      {Type::getInt8PtrTy(_llvmContext), Type::getInt8PtrTy(_llvmContext)},
                                                      /* isVarArg */ false),
                                    Function::ExternalLinkage, 0U, "RhpAssignRef",
                                    _module); // TODO: ExternalLinkage forced as linked from old module
    }
    return llvmFunc;
}

Function* Llvm::getOrCreateRhpCheckedAssignRef()
{
    Function* llvmFunc = _module->getFunction("RhpCheckedAssignRef");
    if (llvmFunc == nullptr)
    {
        llvmFunc = Function::Create(FunctionType::get(Type::getVoidTy(_llvmContext),
                                                      {Type::getInt8PtrTy(_llvmContext), Type::getInt8PtrTy(_llvmContext)},
                                                      /* isVarArg */ false),
                                    Function::ExternalLinkage, 0U, "RhpCheckedAssignRef",
                                    _module); // TODO: ExternalLinkage forced as linked from old module
    }
    return llvmFunc;
}

Function* Llvm::getOrCreateThrowIfNullFunction()
{
    const char* funcName = "nativeaot.throwifnull";
    Function* llvmFunc = _module->getFunction(funcName);
    if (llvmFunc == nullptr)
    {
        llvmFunc =
            Function::Create(FunctionType::get(Type::getVoidTy(_llvmContext),
                                               {Type::getInt8PtrTy(_llvmContext), Type::getInt8PtrTy(_llvmContext)},
                                               /* isVarArg */ false),
                             Function::InternalLinkage, 0U, funcName, _module);

        llvm::IRBuilder<> builder(_llvmContext);
        llvm::BasicBlock* block = llvm::BasicBlock::Create(_llvmContext, "Block", llvmFunc);
        llvm::BasicBlock* throwBlock = llvm::BasicBlock::Create(_llvmContext, "ThrowBlock", llvmFunc);
        llvm::BasicBlock* retBlock = llvm::BasicBlock::Create(_llvmContext, "RetBlock", llvmFunc);

        builder.SetInsertPoint(block);

        builder.CreateCondBr(builder.CreateICmp(llvm::CmpInst::Predicate::ICMP_EQ, llvmFunc->getArg(1),
                                                llvm::ConstantPointerNull::get(Type::getInt8PtrTy(_llvmContext)),
                                                "nullCheck"), throwBlock, retBlock);
        builder.SetInsertPoint(throwBlock);

        buildThrowException(builder, u8"ThrowHelpers", u8"ThrowNullReferenceException", llvmFunc->getArg(0));

        builder.SetInsertPoint(retBlock);
        builder.CreateRetVoid();
    }

    return llvmFunc;
}

llvm::Instruction* Llvm::getCast(llvm::Value* source, Type* targetType)
{
    Type* sourceType = source->getType();
    if (sourceType == targetType)
        return nullptr;

    Type::TypeID sourceTypeID = sourceType->getTypeID();
    Type::TypeID targetTypeId = targetType->getTypeID();

    if (targetTypeId == Type::TypeID::PointerTyID)
    {
        switch (sourceTypeID)
        {
            case Type::TypeID::PointerTyID:
                return new llvm::BitCastInst(source, targetType, "CastPtrToPtr");
            case Type::TypeID::IntegerTyID:
                return new llvm::IntToPtrInst(source, targetType, "CastPtrToInt");
            default:
                failFunctionCompilation();
        }
    }
    if (targetTypeId == Type::TypeID::IntegerTyID)
    {
        switch (sourceTypeID)
        {
            case Type::TypeID::PointerTyID:
                return new llvm::PtrToIntInst(source, targetType, "CastPtrToInt");
            case Type::TypeID::IntegerTyID:
                if (sourceType->getPrimitiveSizeInBits() > targetType->getPrimitiveSizeInBits())
                {
                    return new llvm::TruncInst(source, targetType, "TruncInt");
                }
            default:
                failFunctionCompilation();
        }
    }

    failFunctionCompilation();
}

Value* Llvm::castIfNecessary(Value* source, Type* targetType, llvm::IRBuilder<>* builder)
{
    if (builder == nullptr)
    {
        builder = &_builder;
    }

    llvm::Instruction* castInst = getCast(source, targetType);
    if (castInst == nullptr)
        return source;

    return builder->Insert(castInst);
}

llvm::Value* Llvm::gepOrAddr(Value* addr, unsigned offset)
{
    if (offset == 0)
    {
        return addr;
    }

    return _builder.CreateGEP(addr, _builder.getInt32(offset));
}

// shadow stack moved up to avoid overwriting anything on the stack in the compiling method
llvm::Value* Llvm::getShadowStackForCallee()
{
    unsigned int offset = getTotalLocalOffset();

    return gepOrAddr(_function->getArg(0), offset);
}

DebugMetadata Llvm::getOrCreateDebugMetadata(const char* documentFileName)
{
    std::string fullPath = documentFileName;
    DebugMetadata debugMetadata;
    if (!_debugMetadataMap.Lookup(fullPath, &debugMetadata))
    {
        // check Unix and Windows path styles
        std::size_t botDirPos = fullPath.find_last_of("/");
        if (botDirPos == std::string::npos)
        {
            botDirPos = fullPath.find_last_of("\\");
        }
        std::string directory = ""; // is it possible there is never a directory?
        std::string fileName;
        if (botDirPos != std::string::npos)
        {
            directory = fullPath.substr(0, botDirPos);
            fileName = fullPath.substr(botDirPos + 1, fullPath.length());
        }
        else
        {
            fileName = fullPath;
        }

        _diBuilder                 = new llvm::DIBuilder(*_module);
        llvm::DIFile* fileMetadata = _diBuilder->createFile(fileName, directory);

        llvm::DICompileUnit* compileUnit =
            _diBuilder->createCompileUnit(llvm::dwarf::DW_LANG_C /* no dotnet choices in the enum */, fileMetadata,
                                          "ILC", _compiler->opts.OptimizationEnabled(), "", 1, "",
                                          llvm::DICompileUnit::DebugEmissionKind::FullDebug, 0, 0, 0,
                                          llvm::DICompileUnit::DebugNameTableKind::Default, false, "");

        debugMetadata = {fileMetadata, compileUnit};
        _debugMetadataMap.Set(fullPath, debugMetadata);
    }

    return debugMetadata;
}

llvm::DILocation* Llvm::createDebugFunctionAndDiLocation(DebugMetadata debugMetadata, unsigned int lineNo)
{
    if (_debugFunction == nullptr)
    {
        llvm::DISubroutineType* functionMetaType = _diBuilder->createSubroutineType({} /* TODO - function parameter types*/, llvm::DINode::DIFlags::FlagZero);
        uint32_t lineNumber = FirstSequencePointLineNumber();

        const char* methodName = _info.compCompHnd->getMethodName(_info.compMethodHnd, nullptr);
        _debugFunction = _diBuilder->createFunction(debugMetadata.fileMetadata, methodName,
                                                    methodName, debugMetadata.fileMetadata, lineNumber,
                                                    functionMetaType, lineNumber, llvm::DINode::DIFlags::FlagZero,
                                                    llvm::DISubprogram::DISPFlags::SPFlagDefinition |
                                                    llvm::DISubprogram::DISPFlags::SPFlagLocalToUnit);
        _function->setSubprogram(_debugFunction);
    }
    return llvm::DILocation::get(_llvmContext, lineNo, 0, _debugFunction);
}

llvm::BasicBlock* Llvm::getLLVMBasicBlockForBlock(BasicBlock* block)
{
    llvm::BasicBlock* llvmBlock;
    if (!_blkToLlvmBlkVectorMap.Lookup(block, &llvmBlock))
    {
        unsigned bbNum = block->bbNum;
        llvmBlock = llvm::BasicBlock::Create(_llvmContext, (bbNum >= 10) ? ("BB" + llvm::Twine(bbNum))
                                                                         : ("BB0" + llvm::Twine(bbNum)), _function);
        _blkToLlvmBlkVectorMap.Set(block, llvmBlock);
    }

    return llvmBlock;
}

bool Llvm::isLlvmFrameLocal(LclVarDsc* varDsc)
{
    assert(canStoreLocalOnLlvmStack(varDsc) && (_compiler->fgSsaPassesCompleted >= 1));
    return !varDsc->lvInSsa && varDsc->lvRefCnt() > 0;
}

unsigned int Llvm::getTotalLocalOffset()
{
    assert((_shadowStackLocalsSize % TARGET_POINTER_SIZE) == 0);
    return _shadowStackLocalsSize;
}
