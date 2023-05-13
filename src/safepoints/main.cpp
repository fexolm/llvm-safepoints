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

#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/IR/Module.h>

#include <llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/JITLink/JITLink.h>

class StatepointGC : public llvm::GCStrategy
{
public:
  StatepointGC()
  {
    UseStatepoints = true;
    // These options are all gc.root specific, we specify them so that the
    // gc.root lowering code doesn't run.
    NeededSafePoints = 0;
    UsesMetadata = false;
  }

  llvm::Optional<bool> isGCManagedPointer(const llvm::Type *Ty) const override
  {
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

class MyPlugin : public llvm::orc::ObjectLinkingLayer::Plugin
{
public:
  // Add passes to print the set of defined symbols after dead-stripping.
  void modifyPassConfig(llvm::orc::MaterializationResponsibility &MR,
                        llvm::jitlink::LinkGraph &G,
                        llvm::jitlink::PassConfiguration &Config) override
  {
    Config.PrePrunePasses.push_back([this](llvm::jitlink::LinkGraph &G)
                                     {
      return printAllSymbols(G);
    });
  }

  // Implement mandatory overrides:
  llvm::Error notifyFailed(llvm::orc::MaterializationResponsibility &MR) override
  {
    return llvm::Error::success();
  }
  llvm::Error notifyRemovingResources(llvm::orc::ResourceKey K) override
  {
    return llvm::Error::success();
  }
  void notifyTransferringResources(llvm::orc::ResourceKey DstKey,
                                   llvm::orc::ResourceKey SrcKey) override {}

  // JITLink pass to print all defined symbols in G.
  llvm::Error printAllSymbols(llvm::jitlink::LinkGraph &G)
  {
    llvm::jitlink::Section *Section = G.findSectionByName(".llvm_stackmaps");

    if(Section) {

      llvm::dbgs() << Section->blocks_size() << "\n";
      llvm::dbgs() << Section->symbols_size() << "\n";

      for(auto &block: Section->blocks()) {
        uint8_t *data = (uint8_t *)block->getContent().data();

        using StackMapParser = llvm::StackMapParser<llvm::support::endianness::little>;
        using LocationKind = StackMapParser::LocationKind;
        StackMapParser parser(llvm::ArrayRef<uint8_t>(data, block->getContent().size()));

        for(auto &r: parser.records()) {
          for(auto &l: r.locations()) {
            if(l.getKind() == LocationKind::Direct || l.getKind() == LocationKind::Indirect) {
              llvm::dbgs() << (int)l.getKind() << " ";
              llvm::dbgs() << (int)l.getOffset() << " ";
              llvm::dbgs() << (int)l.getDwarfRegNum();
              llvm::dbgs() << "\n";
            } else {
              llvm::dbgs() << l.getSmallConstant() << "\n";
            }
          }
        }
      }
    }

    return llvm::Error::success();
  }
};

int main()
{
  std::unique_ptr<llvm::LLVMContext> ctx = std::make_unique<llvm::LLVMContext>();

  llvm::IRBuilder builder(*ctx);

  llvm::FunctionType *fnty = llvm::FunctionType::get(llvm::Type::getInt8Ty(*ctx), {llvm::Type::getInt8PtrTy(*ctx, 1), llvm::Type::getInt8PtrTy(*ctx, 1)}, false);

  llvm::FunctionType *f2ty = llvm::FunctionType::get(llvm::Type::getVoidTy(*ctx), {}, false);

  std::unique_ptr<llvm::Module> m = std::make_unique<llvm::Module>("mod", *ctx);

  llvm::Function *f = llvm::Function::Create(fnty, llvm::GlobalValue::LinkageTypes::ExternalLinkage, "f1", m.get());
  f->setGC("statepoint-example");

  llvm::Function *f2 = llvm::Function::Create(f2ty, llvm::GlobalValue::LinkageTypes::ExternalLinkage, "f2", m.get());

  llvm::BasicBlock *body = llvm::BasicBlock::Create(*ctx, "body", f);

  m->setDataLayout("ni:1");

  builder.SetInsertPoint(body);

  builder.CreateCall(f2, {});

  llvm::Value *arg0 = builder.CreateLoad(llvm::Type::getInt8Ty(*ctx), f->getArg(0));
  llvm::Value *arg1 = builder.CreateLoad(llvm::Type::getInt8Ty(*ctx), f->getArg(1));

  builder.CreateRet(builder.CreateAdd(arg0, arg1));

  m->print(llvm::outs(), nullptr);

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

  MPM.run(*m, MAM);

  llvm::BasicBlock *body2 = llvm::BasicBlock::Create(*ctx, "body", f2);
  builder.SetInsertPoint(body2);
  builder.CreateRetVoid();

  m->print(llvm::outs(), nullptr);

  m->setDataLayout("");

  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();

  llvm::ExitOnError ExitOnErr;
  llvm::orc::LLJITBuilder jit_builder;

  jit_builder.setObjectLinkingLayerCreator(
      [](llvm::orc::ExecutionSession &ES, const llvm::Triple &T)
      {
        // Manually set up the ObjectLinkingLayer for our LLJIT
        // instance.
        auto OLL = std::make_unique<llvm::orc::ObjectLinkingLayer>(
            ES, std::make_unique<llvm::jitlink::InProcessMemoryManager>(1024 * 1024 * 1024));

        // Install our plugin:
        OLL->addPlugin(std::make_unique<MyPlugin>());

        return OLL;
      });

  auto jit = ExitOnErr(jit_builder.create());

  llvm::orc::ThreadSafeModule tsm(std::move(m), std::move(ctx));

  ExitOnErr(jit->addIRModule(std::move(tsm)));

  auto ff = ExitOnErr(jit->lookup("f1"));

  ff.getValue();
}