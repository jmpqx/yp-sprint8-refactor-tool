#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Refactoring.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"

#include <unordered_set>

#include "RefactorTool.h"

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::tooling;

static llvm::cl::OptionCategory ToolCategory("refactor-tool options");

// Метод run вызывается для каждого совпадения с матчем.
// Мы проверяем тип совпадения по bind-именам и применяем рефакторинг.
void RefactorHandler::run(const MatchFinder::MatchResult &Result) {
    auto &Diag = Result.Context->getDiagnostics();
    auto &SM = *Result.SourceManager;  // Получаем SourceManager для проверки isInMainFile

    if (const auto *Dtor = Result.Nodes.getNodeAs<CXXDestructorDecl>("classDecl")) {
        handle_nv_dtor(Dtor, Diag, SM);
    }

    if (const auto *Method = Result.Nodes.getNodeAs<CXXMethodDecl>("methodDecl");
        Method && Method->size_overridden_methods() > 0 && !Method->hasAttr<OverrideAttr>()) {
        handle_miss_override(Method, Diag, SM);
    }

    if (const auto *LoopVar = Result.Nodes.getNodeAs<VarDecl>("VarDecl")) {
        handle_crange_for(LoopVar, Diag, SM);
    }
}

void RefactorHandler::handle_nv_dtor(const CXXDestructorDecl *Dtor, DiagnosticsEngine &Diag, SourceManager &SM) {
    if (!SM.isInMainFile(Dtor->getLocation())) {
        return;
    }

    const SourceLocation InsertLoc = Dtor->getBeginLoc();
    if (!virtualDtorLocations.insert(InsertLoc.getRawEncoding()).second) {
        return;
    }

    Rewrite.InsertTextBefore(InsertLoc, "virtual ");

    const unsigned DiagID = Diag.getCustomDiagID(DiagnosticsEngine::Remark, "Деструктор класса %0 сделан виртуальным");
    Diag.Report(Dtor->getLocation(), DiagID) << Dtor->getParent();
}

void RefactorHandler::handle_miss_override(const CXXMethodDecl *Method, DiagnosticsEngine &Diag, SourceManager &SM) {
    if (!SM.isInMainFile(Method->getLocation())) {
        return;
    }

    if (isa<CXXDestructorDecl>(Method)) {
        return;
    }

    if (!Method->getLexicalDeclContext()->isRecord()) {
        return;
    }

    if (!overrideLocations.insert(Method->getLocation().getRawEncoding()).second) {
        return;
    }

    if (Method->doesThisDeclarationHaveABody()) {
        Rewrite.InsertTextBefore(Method->getBody()->getBeginLoc(), "override ");
    } else {
        Rewrite.InsertTextAfterToken(Method->getEndLoc(), " override");
    }

    const unsigned DiagID = Diag.getCustomDiagID(DiagnosticsEngine::Remark, "Метод %0 помечен override");
    Diag.Report(Method->getLocation(), DiagID) << Method;
}

void RefactorHandler::handle_crange_for(const VarDecl *LoopVar, DiagnosticsEngine &Diag, SourceManager &SM) {
    if (!SM.isInMainFile(LoopVar->getLocation())) {
        return;
    }

    const QualType QT = LoopVar->getType();
    if (!QT.isConstQualified() || QT->isReferenceType() || QT.getCanonicalType()->isFundamentalType()) {
        return;
    }

    const TypeSourceInfo *TSI = LoopVar->getTypeSourceInfo();
    if (!TSI) {
        return;
    }

    const SourceLocation TypeEnd = TSI->getTypeLoc().getEndLoc();
    if (!rangeForRefLocations.insert(TypeEnd.getRawEncoding()).second) {
        return;
    }

    Rewrite.InsertTextAfterToken(TypeEnd, "&");

    const unsigned DiagID =
        Diag.getCustomDiagID(DiagnosticsEngine::Remark, "Переменная цикла %0 заменена на константную ссылку");
    Diag.Report(LoopVar->getLocation(), DiagID) << LoopVar;
}

// note: синтаксис написания матчеров точно такой же как и для использования clang-query

auto NvDtorMatcher() {
    return cxxRecordDecl(isExpansionInMainFile(),
                         isDerivedFrom(cxxRecordDecl(
                             has(cxxDestructorDecl(unless(isVirtual()), unless(isImplicit())).bind("classDecl")))));
}

auto NoOverrideMatcher() {
    return cxxMethodDecl(isExpansionInMainFile(), isOverride(), unless(cxxDestructorDecl()), unless(isImplicit()),
                         unless(hasAttr(clang::attr::Override)))
        .bind("methodDecl");
}

auto NoRefConstVarInRangeLoopMatcher() {
    return cxxForRangeStmt(isExpansionInMainFile(),
                           hasLoopVariable(varDecl(hasType(qualType(isConstQualified()))).bind("VarDecl")));
}

// Конструктор принимает Rewriter для изменения кода.
ComplexConsumer::ComplexConsumer(Rewriter &Rewrite) : Handler(Rewrite) {
    // Создаем MatchFinder и добавляем матчеры.
    Finder.addMatcher(NvDtorMatcher(), &Handler);
    Finder.addMatcher(NoOverrideMatcher(), &Handler);
    Finder.addMatcher(NoRefConstVarInRangeLoopMatcher(), &Handler);
}

// Метод HandleTranslationUnit вызывается для каждого файла.
void ComplexConsumer::HandleTranslationUnit(ASTContext &Context) { Finder.matchAST(Context); }

std::unique_ptr<ASTConsumer> CodeRefactorAction::CreateASTConsumer(CompilerInstance &CI, StringRef file) {
    RewriterForCodeRefactor.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
    return std::make_unique<ComplexConsumer>(RewriterForCodeRefactor);
}

bool CodeRefactorAction::BeginSourceFileAction(CompilerInstance &CI) {
    // Инициализируем Rewriter для рефакторинга.
    RewriterForCodeRefactor.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
    return true;  // Возвращаем true, чтобы продолжить обработку файла.
}

void CodeRefactorAction::EndSourceFileAction() {
    // Применяем изменения в файле.
    if (RewriterForCodeRefactor.overwriteChangedFiles()) {
        llvm::errs() << "Error applying changes to files.\n";
    }
}

int main(int argc, const char **argv) {
    // Парсер опций: Обрабатывает флаги командной строки, компиляционные базы данных.
    auto ExpectedParser = CommonOptionsParser::create(argc, argv, ToolCategory);
    if (!ExpectedParser) {
        llvm::errs() << ExpectedParser.takeError();
        return 1;
    }
    CommonOptionsParser &OptionsParser = ExpectedParser.get();
    // Создаем ClangTool
    ClangTool Tool(OptionsParser.getCompilations(), OptionsParser.getSourcePathList());
    // Запускаем RefactorAction.
    return Tool.run(newFrontendActionFactory<CodeRefactorAction>().get());
}