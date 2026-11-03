/* Stand-in for LLVM's llvm-c/ExternC.h, which the Windows LLVM installer does
   not ship although its llvm-c/Remarks.h includes it. Same macros as LLVM's. */
#ifndef LLVM_C_EXTERNC_H
#define LLVM_C_EXTERNC_H

#ifdef __cplusplus
#define LLVM_C_EXTERN_C_BEGIN extern "C" {
#define LLVM_C_EXTERN_C_END }
#else
#define LLVM_C_EXTERN_C_BEGIN
#define LLVM_C_EXTERN_C_END
#endif

#endif
