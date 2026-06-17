#include <gtest/gtest.h>

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path ExeDir() {
    std::error_code EC;
    const fs::path Self = fs::read_symlink("/proc/self/exe", EC);
    return EC ? fs::current_path() : Self.parent_path();
}

fs::path TestsDataDir() { return ExeDir().parent_path() / "tests" / "tests_data"; }

std::string ReadFile(const fs::path &Path) {
    std::ifstream In(Path);
    std::ostringstream Content;
    Content << In.rdbuf();
    return Content.str();
}

// Копирует заготовку из tests_data в tests_data/tmp, запускает на копии
// refactor_tool и возвращает содержимое файла после рефакторинга.
std::string RefactorTestFile(const std::string &SourceName, const std::string &TmpName) {
    const fs::path Source = TestsDataDir() / SourceName;
    const fs::path Tmp = TestsDataDir() / "tmp" / TmpName;
    fs::create_directories(Tmp.parent_path());
    fs::copy_file(Source, Tmp, fs::copy_options::overwrite_existing);

    const std::string Cmd = (ExeDir() / "refactor_tool").string() + " \"" + Tmp.string() + "\" -- 2>/dev/null";
    EXPECT_EQ(std::system(Cmd.c_str()), 0) << "refactor_tool завершился с ошибкой";

    return ReadFile(Tmp);
}

size_t CountOccurrences(const std::string &Haystack, const std::string &Needle) {
    size_t Count = 0;
    for (size_t Pos = Haystack.find(Needle); Pos != std::string::npos;
         Pos = Haystack.find(Needle, Pos + Needle.size())) {
        ++Count;
    }
    return Count;
}

// Сравнение без учёта пробелов и пустых строк — как diff -w -B в check_refactor.sh.
std::string NormalizeWhitespace(const std::string &Text) {
    std::string Result;
    std::istringstream In(Text);
    std::string Line;
    while (std::getline(In, Line)) {
        std::string Stripped;
        for (const char C : Line) {
            if (!std::isspace(static_cast<unsigned char>(C))) {
                Stripped += C;
            }
        }
        if (!Stripped.empty()) {
            Result += Stripped;
            Result += '\n';
        }
    }
    return Result;
}

std::string ReadReference(const std::string &RefName) { return ReadFile(TestsDataDir() / RefName); }

}  // namespace

//
// 1. Невиртуальные деструкторы (tests_data/test1.cpp)
//

TEST(NvDtorRefactor, MatchesReference) {
    const std::string Result = RefactorTestFile("test1.cpp", "unit_test1_add.cpp");

    EXPECT_EQ(NormalizeWhitespace(Result), NormalizeWhitespace(ReadReference("test1_ref.cpp"))) << Result;
}

TEST(NvDtorRefactor, NoFalseChanges) {
    const std::string Result = RefactorTestFile("test1.cpp", "unit_test1_neg.cpp");

    // Класс без наследников не меняется.
    EXPECT_NE(Result.find("~Standalone() {}"), std::string::npos) << Result;
    EXPECT_EQ(Result.find("virtual ~Standalone"), std::string::npos) << Result;
    // Два добавленных virtual + один исходный у BaseVirtual, без дублей.
    EXPECT_EQ(CountOccurrences(Result, "virtual ~"), 3u) << Result;
}

//
// 2. Отсутствующий override (tests_data/test2.cpp)
//

TEST(OverrideRefactor, MatchesReference) {
    const std::string Result = RefactorTestFile("test2.cpp", "unit_test2_add.cpp");

    EXPECT_EQ(NormalizeWhitespace(Result), NormalizeWhitespace(ReadReference("test2_ref.cpp"))) << Result;
}

TEST(OverrideRefactor, NoFalseChanges) {
    const std::string Result = RefactorTestFile("test2.cpp", "unit_test2_neg.cpp");

    // Деструктор наследника не помечается override.
    EXPECT_NE(Result.find("~Derived() {}"), std::string::npos) << Result;
    EXPECT_EQ(Result.find("~Derived() override"), std::string::npos) << Result;
    // 4 добавленных + 1 исходный у DerivedWithOverride, без дублей.
    EXPECT_EQ(CountOccurrences(Result, "override {}"), 5u) << Result;
    EXPECT_EQ(Result.find("override override"), std::string::npos) << Result;
}

//
// 3. range-for без const & (tests_data/test3.cpp)
//

TEST(RangeForRefactor, MatchesReference) {
    const std::string Result = RefactorTestFile("test3.cpp", "unit_test3_add.cpp");

    EXPECT_EQ(NormalizeWhitespace(Result), NormalizeWhitespace(ReadReference("test3_ref.cpp"))) << Result;
}

TEST(RangeForRefactor, NoFalseChanges) {
    const std::string Result = RefactorTestFile("test3.cpp", "unit_test3_neg.cpp");

    // Фундаментальный тип не меняется.
    EXPECT_NE(Result.find("for (const int x : ints)"), std::string::npos) << Result;
    // Одна ссылка добавлена, одна была в исходнике, лишних & нет.
    EXPECT_EQ(CountOccurrences(Result, "const auto& x : vec"), 2u) << Result;
    EXPECT_EQ(Result.find("&&"), std::string::npos) << Result;
}

//
// Все три рефакторинга в одном файле (tests_data/for_refactor.cpp)
//

TEST(CombinedRefactor, AppliesAllRefactoringsToOneFile) {
    const std::string Result = RefactorTestFile("for_refactor.cpp", "unit_for_refactor.cpp");

    EXPECT_NE(Result.find("virtual ~Base() {}"), std::string::npos) << Result;
    EXPECT_NE(Result.find("virtual void method() override {};"), std::string::npos) << Result;
    // vector<int> — фундаментальный тип, не меняется.
    EXPECT_NE(Result.find("for (const auto x1 : v1)"), std::string::npos) << Result;
    EXPECT_NE(Result.find("for (const auto& x12 : vec)"), std::string::npos) << Result;
}
