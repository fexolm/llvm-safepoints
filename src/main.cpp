#include <llvm/IR/Statepoint.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Type.h>

#include <llvm/Transforms/Scalar.h>
#include <llvm/InitializePasses.h>

#include <llvm/Passes/PassBuilder.h>

#include <llvm/IR/LegacyPassManager.h>

#include <iostream>

#include <llvm/Transforms/Scalar/RewriteStatepointsForGC.h>

#include <llvm/Object/StackMapParser.h>

#include <llvm/Support/TargetSelect.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/Support/Host.h>
#include <llvm/Target/TargetMachine.h>

#include <llvm/CodeGen/GCMetadataPrinter.h>
#include <llvm/CodeGen/GCMetadata.h>

#include <llvm/CodeGen/AsmPrinter.h>
#include <llvm/CodeGen/TargetLoweringObjectFileImpl.h>

#include <llvm/MC/MCStreamer.h>
#include <llvm/MC/MCContext.h>
#include <llvm/MC/MCSection.h>
#include <llvm/MC/MCSectionELF.h>

#include <llvm/IR/GCStrategy.h>


class StatepointGC : public llvm::GCStrategy {
public:
  StatepointGC() {
    UseStatepoints = true;
    // These options are all gc.root specific, we specify them so that the
    // gc.root lowering code doesn't run.
    NeededSafePoints = 0;
    UsesMetadata = false;
  }

  llvm::Optional<bool> isGCManagedPointer(const llvm::Type *Ty) const override {
    // Method is only valid on pointer typed values.
    const llvm::PointerType *PT = llvm::cast<llvm::PointerType>(Ty);
    // For the sake of this example GC, we arbitrarily pick addrspace(1) as our
    // GC managed heap.  We know that a pointer into this heap needs to be
    // updated and that no other pointer does.  Note that addrspace(1) is used
    // only as an example, it has no special meaning, and is not reserved for
    // GC usage.
    return (1 == PT->getAddressSpace());
  }
};

static llvm::GCRegistry::Add<StatepointGC> D("statepoint-example",
                                       "an example strategy for statepoint");


int main() {
    llvm::LLVMContext ctx;

    llvm::IRBuilder builder(ctx);

    llvm::FunctionType *fnty = llvm::FunctionType::get(llvm::Type::getInt8Ty(ctx), {llvm::Type::getInt8PtrTy(ctx, 1), llvm::Type::getInt8PtrTy(ctx, 1)}, false);

    llvm::FunctionType *f2ty = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), {}, false);

    llvm::Module m("mod", ctx);

    llvm::Function *f = llvm::Function::Create(fnty, llvm::GlobalValue::LinkageTypes::ExternalLinkage, "f1", &m);
    f->setGC("statepoint-example");

    llvm::Function *f2 = llvm::Function::Create(f2ty, llvm::GlobalValue::LinkageTypes::ExternalLinkage, "f2", &m);

    llvm::BasicBlock *body = llvm::BasicBlock::Create(ctx, "body", f);

    m.setDataLayout("ni:1");

    std::cout << m.getDataLayoutStr() << std::endl;

    builder.SetInsertPoint(body);

    builder.CreateCall(f2, {});

    llvm::Value *arg0 = builder.CreateLoad(llvm::Type::getInt8Ty(ctx), f->getArg(0));
    llvm::Value *arg1 = builder.CreateLoad(llvm::Type::getInt8Ty(ctx), f->getArg(1));

    builder.CreateRet(builder.CreateAdd(arg0, arg1));

    m.print(llvm::outs(), nullptr);

    llvm::LoopAnalysisManager LAM;
    llvm::FunctionAnalysisManager FAM;
    llvm::CGSCCAnalysisManager CGAM;
    llvm::ModuleAnalysisManager MAM;

    llvm::PassBuilder PB;

    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

    llvm::ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O3);

    llvm::FunctionPassManager FPM;

    MPM.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(FPM)));

    MPM.addPass(llvm::RewriteStatepointsForGC());

    MPM.run(m, MAM);

    m.print(llvm::outs(), nullptr);

    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();

    auto TargetTriple = llvm::sys::getDefaultTargetTriple();
    std::string Error;

    auto Target = llvm::TargetRegistry::lookupTarget(TargetTriple, Error);

    auto CPU = "generic";
    auto Features = "";

    llvm::TargetOptions opt;
    auto RM = llvm::Optional<llvm::Reloc::Model>();
    llvm::TargetMachine *TargetMachine = Target->createTargetMachine(TargetTriple, CPU, Features, opt, RM);

    auto Filename = "output.o";
    std::error_code EC;
    llvm::raw_fd_ostream dest(Filename, EC);

    if (EC) {
        llvm::errs() << "Could not open file: " << EC.message();
        return 1;
    }

    llvm::legacy::PassManager pass;

    auto FileType = llvm::CGFT_AssemblyFile;

    if (TargetMachine->addPassesToEmitFile(pass, dest, nullptr, FileType)) {
        llvm::errs() << "TargetMachine can't emit a file of this type";
        return 1;
    }

    pass.run(m);
    dest.flush();
}