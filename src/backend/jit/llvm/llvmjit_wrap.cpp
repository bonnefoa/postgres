/*-------------------------------------------------------------------------
 *
 * llvmjit_wrap.cpp
 *	  Parts of the LLVM interface not (yet) exposed to C.
 *
 * Copyright (c) 2016-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/lib/llvm/llvmjit_wrap.cpp
 *
 *-------------------------------------------------------------------------
 */

extern "C"
{
#include "postgres.h"
}

#include <llvm-c/Core.h>

/* Avoid macro clash with LLVM's C++ headers */
#undef Min

#include <llvm/IR/Attributes.h>
#include <llvm/IR/Function.h>
#if LLVM_VERSION_MAJOR < 17
#include <llvm/MC/SubtargetFeature.h>
#endif
#if LLVM_VERSION_MAJOR > 16
#include <llvm/TargetParser/Host.h>
#else
#include <llvm/Support/Host.h>
#endif

#include "jit/llvmjit.h"
#ifdef USE_JITLINK
#include "llvm/ExecutionEngine/JITLink/EHFrameSupport.h"
#include "llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h"
#if LLVM_VERSION_MAJOR > 17
#include "llvm/ExecutionEngine/Orc/Debugging/DebuggerSupportPlugin.h"
#else
#include "llvm/ExecutionEngine/Orc/DebuggerSupportPlugin.h"
#endif
#endif

/*
 * C-API extensions.
 */
#if defined(HAVE_DECL_LLVMGETHOSTCPUNAME) && !HAVE_DECL_LLVMGETHOSTCPUNAME
char *LLVMGetHostCPUName(void) {
	return strdup(llvm::sys::getHostCPUName().data());
}
#endif


#if defined(HAVE_DECL_LLVMGETHOSTCPUFEATURES) && !HAVE_DECL_LLVMGETHOSTCPUFEATURES
char *LLVMGetHostCPUFeatures(void) {
	llvm::SubtargetFeatures Features;
	llvm::StringMap<bool> HostFeatures;

	if (llvm::sys::getHostCPUFeatures(HostFeatures))
		for (auto &F : HostFeatures)
			Features.AddFeature(F.first(), F.second);

	return strdup(Features.getString().c_str());
}
#endif

/*
 * Like LLVM's LLVMGetAttributeCountAtIndex(), works around a bug in LLVM 3.9.
 *
 * In LLVM <= 3.9, LLVMGetAttributeCountAtIndex() segfaults if there are no
 * attributes at an index (fixed in LLVM commit ce9bb1097dc2).
 */
unsigned
LLVMGetAttributeCountAtIndexPG(LLVMValueRef F, uint32 Idx)
{
	/*
	 * This is more expensive, so only do when using a problematic LLVM
	 * version.
	 */
#if LLVM_VERSION_MAJOR < 4
	if (!llvm::unwrap<llvm::Function>(F)->getAttributes().hasAttributes(Idx))
		return 0;
#endif

	/*
	 * There is no nice public API to determine the count nicely, so just
	 * always fall back to LLVM's C API.
	 */
	return LLVMGetAttributeCountAtIndex(F, Idx);
}

LLVMTypeRef
LLVMGetFunctionReturnType(LLVMValueRef r)
{
	return llvm::wrap(llvm::unwrap<llvm::Function>(r)->getReturnType());
}

LLVMTypeRef
LLVMGetFunctionType(LLVMValueRef r)
{
	return llvm::wrap(llvm::unwrap<llvm::Function>(r)->getFunctionType());
}

#if LLVM_VERSION_MAJOR < 8
LLVMTypeRef
LLVMGlobalGetValueType(LLVMValueRef g)
{
	return llvm::wrap(llvm::unwrap<llvm::GlobalValue>(g)->getValueType());
}
#endif

#ifdef USE_JITLINK
/*
 * There is no public C API to create ObjectLinkingLayer for JITLINK, create our own
 */
DEFINE_SIMPLE_CONVERSION_FUNCTIONS(llvm::orc::ExecutionSession, LLVMOrcExecutionSessionRef)
DEFINE_SIMPLE_CONVERSION_FUNCTIONS(llvm::orc::ObjectLinkingLayer, LLVMOrcObjectLayerRef)
DEFINE_SIMPLE_CONVERSION_FUNCTIONS(llvm::orc::JITDylib , LLVMOrcJITDylibRef)

LLVMOrcObjectLayerRef
LLVMOrcCreateJitlinkObjectLinkingLayer(LLVMOrcExecutionSessionRef ES)
{
	Assert(ES);
	auto *ObjLinkingLayer = new llvm::orc::ObjectLinkingLayer(*unwrap(ES));
	ObjLinkingLayer->addPlugin(std::make_unique<llvm::orc::EHFrameRegistrationPlugin>(
		*unwrap(ES), std::make_unique<llvm::jitlink::InProcessEHFrameRegistrar>()));
	return wrap(ObjLinkingLayer);
}

LLVMErrorRef
LLVMOrcAddGDBPluginObjectLinkingLayer(LLVMOrcObjectLayerRef OL, LLVMOrcExecutionSessionRef ES, LLVMOrcJITDylibRef JD, const char *triple)
{
    auto GDBPlugin = llvm::orc::GDBJITDebugInfoRegistrationPlugin::Create(
        *unwrap(ES), *unwrap(JD), llvm::Triple(triple));
    if (!GDBPlugin) {
        return wrap(GDBPlugin.takeError());
    }
	unwrap(OL)->addPlugin(std::move(*GDBPlugin));
    return LLVMErrorSuccess;
}

#endif
