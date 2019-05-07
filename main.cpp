#include <iostream>

#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Sema/Sema.h"
#include "clang/Basic/PartialDiagnostic.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "clang/Basic/DiagnosticSema.h"
#include "llvm/Support/CommandLine.h"
#include "clang/Rewrite/Frontend/FixItRewriter.h"


using namespace clang;
using namespace clang::tooling;

namespace {


    bool CheckConstexprDeclStmt(Sema &SemaRef, const FunctionDecl *Dcl,
                                DeclStmt *DS, SourceLocation &Cxx1yLoc) {
        // C++11 [dcl.constexpr]p3 and p4:
        //  The definition of a constexpr function(p3) or constructor(p4) [...] shall
        //  contain only
        for (const auto *DclIt : DS->decls()) {
            switch (DclIt->getKind()) {
                case Decl::StaticAssert:
                case Decl::Using:
                case Decl::UsingShadow:
                case Decl::UsingDirective:
                case Decl::UnresolvedUsingTypename:
                case Decl::UnresolvedUsingValue:
                    //   - static_assert-declarations
                    //   - using-declarations,
                    //   - using-directives,
                    continue;

                case Decl::Typedef:
                case Decl::TypeAlias: {
                    //   - typedef declarations and alias-declarations that do not define
                    //     classes or enumerations,
                    const auto *TN = cast<TypedefNameDecl>(DclIt);
                    if (TN->getUnderlyingType()->isVariablyModifiedType()) {
                        // Don't allow variably-modified types in constexpr functions.
                        TypeLoc TL = TN->getTypeSourceInfo()->getTypeLoc();
                        SemaRef.Diag(TL.getBeginLoc(), diag::err_constexpr_vla)
                                << TL.getSourceRange() << TL.getType()
                                << isa<CXXConstructorDecl>(Dcl);
                        return false;
                    }
                    continue;
                }

                case Decl::Enum:
                case Decl::CXXRecord:
                    // C++1y allows types to be defined, not just declared.
                    if (cast<TagDecl>(DclIt)->isThisDeclarationADefinition())
                        SemaRef.Diag(DS->getBeginLoc(),
                                     SemaRef.getLangOpts().CPlusPlus14
                                     ? diag::warn_cxx11_compat_constexpr_type_definition
                                     : diag::ext_constexpr_type_definition)
                                << isa<CXXConstructorDecl>(Dcl);
                    continue;

                case Decl::EnumConstant:
                case Decl::IndirectField:
                case Decl::ParmVar:
                    // These can only appear with other declarations which are banned in
                    // C++11 and permitted in C++1y, so ignore them.
                    continue;

                case Decl::Var:
                case Decl::Decomposition: {
                    // C++1y [dcl.constexpr]p3 allows anything except:
                    //   a definition of a variable of non-literal type or of static or
                    //   thread storage duration or for which no initialization is performed.
                    const auto *VD = cast<VarDecl>(DclIt);
                    if (VD->isThisDeclarationADefinition()) {
                        if (VD->isStaticLocal()) {
                            SemaRef.Diag(VD->getLocation(),
                                         diag::err_constexpr_local_var_static)
                                    << isa<CXXConstructorDecl>(Dcl)
                                    << (VD->getTLSKind() == VarDecl::TLS_Dynamic);
                            return false;
                        }
                        if (!VD->getType()->isDependentType() &&
                            SemaRef.RequireLiteralType(
                                    VD->getLocation(), VD->getType(),
                                    diag::err_constexpr_local_var_non_literal_type,
                                    isa<CXXConstructorDecl>(Dcl)))
                            return false;
                        if (!VD->getType()->isDependentType() &&
                            !VD->hasInit() && !VD->isCXXForRangeDecl()) {
                            SemaRef.Diag(VD->getLocation(),
                                         diag::err_constexpr_local_var_no_init)
                                    << isa<CXXConstructorDecl>(Dcl);
                            return false;
                        }
                    }
                    SemaRef.Diag(VD->getLocation(),
                                 SemaRef.getLangOpts().CPlusPlus14
                                 ? diag::warn_cxx11_compat_constexpr_local_var
                                 : diag::ext_constexpr_local_var)
                            << isa<CXXConstructorDecl>(Dcl);
                    continue;
                }

                case Decl::NamespaceAlias:
                case Decl::Function:
                    // These are disallowed in C++11 and permitted in C++1y. Allow them
                    // everywhere as an extension.
                    if (!Cxx1yLoc.isValid())
                        Cxx1yLoc = DS->getBeginLoc();
                    continue;

                default:
                    SemaRef.Diag(DS->getBeginLoc(), diag::err_constexpr_body_invalid_stmt)
                            << isa<CXXConstructorDecl>(Dcl);
                    return false;
            }
        }

        return true;
    }

    // CheckConstexprParameterTypes - Check whether a function's parameter types
    // are all literal types. If so, return true. If not, produce a suitable
    // diagnostic and return false.
    static bool CheckConstexprParameterTypes(Sema &SemaRef,
                                             const FunctionDecl *FD) {
        unsigned ArgIndex = 0;
        const FunctionProtoType *FT = FD->getType()->getAs<FunctionProtoType>();
        for (FunctionProtoType::param_type_iterator i = FT->param_type_begin(),
                     e = FT->param_type_end();
             i != e; ++i, ++ArgIndex) {
            const ParmVarDecl *PD = FD->getParamDecl(ArgIndex);
            SourceLocation ParamLoc = PD->getLocation();
            if (!(*i)->isDependentType() &&
                SemaRef.RequireLiteralType(ParamLoc, *i,
                                           diag::err_constexpr_non_literal_param,
                                           ArgIndex+1, PD->getSourceRange(),
                                           isa<CXXConstructorDecl>(FD)))
                return false;
        }
        return true;
    }
}

/*
 * ConstexprFunctionASTVisitor
 *
 * Find all functions that can be constexpr but arent. Create diagnostics for them and
 * mark them constexpr for the next pass.
 */
class ConstexprFunctionASTVisitor : public clang::RecursiveASTVisitor<ConstexprFunctionASTVisitor> {
    clang::SourceManager &sourceManager_;
    clang::CompilerInstance &CI_;
    clang::DiagnosticsEngine &DE;

public:
    explicit ConstexprFunctionASTVisitor(clang::SourceManager &sm, clang::CompilerInstance &ci)
            : sourceManager_(sm), CI_(ci), DE(ci.getASTContext().getDiagnostics()) {
    }

    bool VisitFunctionDecl(clang::FunctionDecl *func) {

        // Only functions in our TU
        SourceLocation loc = func->getSourceRange().getBegin();
        if (!sourceManager_.isWrittenInMainFile(loc))
            return true;

        // Skip existing constExpr functions
        if (func->isConstexpr())
            return true;

        // Don't mark main as constexpr
        if (func->isMain())
            return true;

        auto &sema = CI_.getSema();

        // Temporarily disable diagnostics for these next functions, use a unique_ptr deleter to handle restoring it
        sema.getDiagnostics().setSuppressAllDiagnostics(true);
        {
            auto returnDiagnostics = [&sema](int *) {
                sema.getDiagnostics().setSuppressAllDiagnostics(false);
            };
            int lol = 0;
            std::unique_ptr<int, decltype(returnDiagnostics)> scope(&lol, returnDiagnostics);

            if (!sema.CheckConstexprFunctionDecl(func)) {
                return true;
            }

            if (!sema.CheckConstexprFunctionBody(func, func->getBody())) {
                return true;
            }

            if (!CheckConstexprParameterTypes(sema, func)) {
                return true;
            }
        }

        // Mark function as constexpr, the next ast visitor will use this information to find constexpr vardecls
        func->setConstexpr(true);

        // Create diagnostic
        const auto FixIt = clang::FixItHint::CreateInsertion(loc, "constexpr ");
        const auto ID =
                DE.getCustomDiagID(clang::DiagnosticsEngine::Warning,
                                   "function can be constexpr");

        DE.Report(loc, ID).AddFixItHint(FixIt);

        return true;
    }

};

class ConstexprVarDeclFunctionASTVisitor : public clang::RecursiveASTVisitor<ConstexprVarDeclFunctionASTVisitor> {
    clang::SourceManager &sourceManager_;
    clang::CompilerInstance &CI_;
    clang::DiagnosticsEngine &DE;

    class ConstexprVarDeclVisitor : public clang::RecursiveASTVisitor<ConstexprVarDeclVisitor> {
        clang::CompilerInstance &CI_;
        clang::DiagnosticsEngine &DE;

    public:
        explicit ConstexprVarDeclVisitor(clang::CompilerInstance &ci)
                :  CI_(ci), DE(ci.getASTContext().getDiagnostics()) {}


        bool VisitDeclStmt(clang::DeclStmt *stmt) {
            if (!stmt->isSingleDecl()) {
                return true;
            }

            clang::VarDecl *var = clang::dyn_cast<clang::VarDecl>(*stmt->decl_begin());
            if (!var) {
                return true;
            }

            if (var->isConstexpr()) {
                return true;
            }

            clang::SourceLocation loc = stmt->getSourceRange().getBegin();
            auto &sema = CI_.getSema();

            Expr *Init = var->getInit();
            if (!Init) {
                return true;
            }

            if (!var->checkInitIsICE()) {
                return true;
            }

            if (Init->isValueDependent()) {
                return true;
            }

            if (!var->evaluateValue()) {
                return true;
            }

            if (!var->isInitICE()) {
                return true;
            }

            const auto FixIt = clang::FixItHint::CreateInsertion(loc, "constexpr ");
            const auto ID =
                    DE.getCustomDiagID(clang::DiagnosticsEngine::Warning,
                                       "variable can be constexpr");

            DE.Report(loc, ID).AddFixItHint(FixIt);

            return true;
        }

    };

public:
    explicit ConstexprVarDeclFunctionASTVisitor(clang::SourceManager &sm, clang::CompilerInstance &ci)
    : sourceManager_(sm), CI_(ci), DE(ci.getASTContext().getDiagnostics()) {
    }

    bool VisitFunctionDecl(clang::FunctionDecl *func) {
        // Only functions in our TU
        SourceLocation loc = func->getSourceRange().getBegin();
        if (!sourceManager_.isWrittenInMainFile(loc))
            return true;

        // Don't go through functions that are already constexpr
        if (func->isConstexpr())
            return true;

        ConstexprVarDeclVisitor vd(CI_);
        vd.TraverseFunctionDecl(func);

        return true;
    }
};

class ConstexprEverythingASTConsumer : public clang::ASTConsumer {
    ConstexprFunctionASTVisitor functionVisitor;
    ConstexprVarDeclFunctionASTVisitor varDeclVisitor;

public:
    // override the constructor in order to pass CI
    explicit ConstexprEverythingASTConsumer(clang::CompilerInstance &ci)
            : functionVisitor(ci.getSourceManager(), ci),
            varDeclVisitor(ci.getSourceManager(), ci) {
    }

    void HandleTranslationUnit(clang::ASTContext &astContext) override {
        functionVisitor.TraverseDecl(astContext.getTranslationUnitDecl());
        varDeclVisitor.TraverseDecl(astContext.getTranslationUnitDecl());
    }
};

class FunctionDeclFrontendAction : public clang::ASTFrontendAction {
public:
    std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance &CI, clang::StringRef file) override {
        return std::make_unique<ConstexprEverythingASTConsumer>(CI); // pass CI pointer to ASTConsumer
    }
};


int main(int argc, const char **argv) {
    llvm::cl::OptionCategory MyToolCategory("constexpr-everything options");
    CommonOptionsParser OptionsParser(argc, argv, MyToolCategory);

    ClangTool Tool(OptionsParser.getCompilations(), OptionsParser.getSourcePathList());
    Tool.run(newFrontendActionFactory<FunctionDeclFrontendAction>().get());

}