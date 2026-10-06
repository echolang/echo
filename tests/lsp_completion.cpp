#include <catch2/catch_test_macros.hpp>

#include <AST/ASTFile.h>
#include <Compiler/DriverOptions.h>
#include <Compiler/Lsp/LspCompletion.h>
#include <Compiler/Lsp/LspSession.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
    Compiler::DriverOptions lsp_driver()
    {
        Compiler::DriverOptions driver;
        driver.subcommand = Compiler::Subcommand::t_lsp;
        driver.no_stdlib = true;
        return driver;
    }

    // completion at `|` in `marked`, which is the editor's text now. the session's snapshot is
    // whatever was compiled last, usually a different text, which is the point
    Compiler::Lsp::CompletionAnswer complete(
        Compiler::Lsp::Session &session,
        const std::filesystem::path &path,
        const std::string &marked,
        bool trigger = false
    )
    {
        const size_t cursor = marked.find('|');
        REQUIRE(cursor != std::string::npos);
        std::string text = marked;
        text.erase(cursor, 1);
        session.did_change(path, 2, text);

        const Compiler::Lsp::LiveText live(text);
        Compiler::Lsp::CompletionRequest request;
        request.live = &live;
        request.cursor = cursor;
        request.from_trigger_character = trigger;
        request.snippets = true;
        if (const AST::File *file = session.file_of(path)) {
            request.snapshot = session.snapshot();
            request.file = file;
            request.map = Compiler::Lsp::LineMap::build(*file, live);
        }

        return Compiler::Lsp::completion(request);
    }

    std::vector<std::string> labels(const Compiler::Lsp::CompletionAnswer &answer)
    {
        std::vector<std::string> out;
        for (const auto &item : answer.items) {
            out.push_back(item.label);
        }
        return out;
    }

    bool has(const Compiler::Lsp::CompletionAnswer &answer, const std::string &label)
    {
        const auto all = labels(answer);
        return std::find(all.begin(), all.end(), label) != all.end();
    }

    const Compiler::Lsp::CompletionItem *item_of(const Compiler::Lsp::CompletionAnswer &answer, const std::string &label)
    {
        for (const auto &item : answer.items) {
            if (item.label == label) {
                return &item;
            }
        }
        return nullptr;
    }

    const std::string k_point
        = "struct Point\n"
          "{\n"
          "    int32 $x;\n"
          "    int32 $y;\n"
          "    private int32 $secret = 0;\n"
          "\n"
          "    function len() : int32 { return $this->x + $this->y; }\n"
          "    function peek() : int32 { return $this->secret; }\n"
          "}\n";
};

TEST_CASE("completion: members of a local, typed after the last compile", "[lsp]")
{
    const Compiler::DriverOptions driver = lsp_driver();
    Compiler::Lsp::Session session(driver);
    const std::filesystem::path path = "/tmp/lsp-complete-members.eco";
    const std::string before
        = k_point
        + "function main() : void {\n"
          "    Point $p = Point(1, 2);\n"
          "    echo $p->x;\n"
          "}\n";
    session.did_open(path, 1, before);
    session.rebuild();

    const auto answer = complete(session, path,
        k_point
        + "function main() : void {\n"
          "    Point $p = Point(1, 2);\n"
          "    echo $p->|\n"
          "}\n");

    REQUIRE(has(answer, "x"));
    REQUIRE(has(answer, "y"));
    REQUIRE(has(answer, "len"));
    REQUIRE_FALSE(has(answer, "secret"));

    const auto *len = item_of(answer, "len");
    REQUIRE(len != nullptr);
    REQUIRE(len->kind == Compiler::Lsp::CompletionItemKind::t_method);
    REQUIRE(len->label_description == "int32");
    REQUIRE(len->insert_text == "len($0)");
    REQUIRE(len->is_snippet);
}

TEST_CASE("completion: a private property is offered inside its own type", "[lsp]")
{
    const Compiler::DriverOptions driver = lsp_driver();
    Compiler::Lsp::Session session(driver);
    const std::filesystem::path path = "/tmp/lsp-complete-private.eco";
    session.did_open(path, 1, k_point);
    session.rebuild();

    std::string marked = k_point;
    marked.replace(marked.find("$this->secret"), std::string("$this->secret").size(), "$this->|");
    const auto answer = complete(session, path, marked);

    REQUIRE(has(answer, "secret"));
    REQUIRE(has(answer, "x"));
}

TEST_CASE("completion: locals keep their types when lines above them moved", "[lsp]")
{
    const Compiler::DriverOptions driver = lsp_driver();
    Compiler::Lsp::Session session(driver);
    const std::filesystem::path path = "/tmp/lsp-complete-shift.eco";
    const std::string body
        = "function main() : void {\n"
          "    $p = Point(1, 2);\n"
          "    $n = $p->len();\n";
    session.did_open(path, 1, k_point + body + "}\n");
    session.rebuild();

    const auto answer = complete(session, path,
        "// one\n// two\n// three\n" + k_point + body + "    $|\n}\n");

    const auto *p = item_of(answer, "$p");
    REQUIRE(p != nullptr);
    REQUIRE(p->label_description == "Point");
    const auto *n = item_of(answer, "$n");
    REQUIRE(n != nullptr);
    REQUIRE(n->label_description == "int32");
}

TEST_CASE("completion: a variable declared in the unsaved edit is typed from its initializer", "[lsp]")
{
    const Compiler::DriverOptions driver = lsp_driver();
    Compiler::Lsp::Session session(driver);
    const std::filesystem::path path = "/tmp/lsp-complete-fresh.eco";
    session.did_open(path, 1, k_point + "function main() : void {\n}\n");
    session.rebuild();

    const auto answer = complete(session, path,
        k_point
        + "function main() : void {\n"
          "    $fresh = Point(1, 2);\n"
          "    $fresh->|\n"
          "}\n");

    REQUIRE(has(answer, "x"));
    REQUIRE(has(answer, "len"));
}

TEST_CASE("completion: variables follow scope and the replace range starts at the sigil", "[lsp]")
{
    const Compiler::DriverOptions driver = lsp_driver();
    Compiler::Lsp::Session session(driver);
    const std::filesystem::path path = "/tmp/lsp-complete-scope.eco";
    const std::string source
        = "$global = 1;\n"
          "function other(int32 $nope) : void {}\n"
          "function f(int32 $a) : void {\n"
          "    int32 $b = 2;\n"
          "}\n";
    session.did_open(path, 1, source);
    session.rebuild();

    const std::string marked
        = "$global = 1;\n"
          "function other(int32 $nope) : void {}\n"
          "function f(int32 $a) : void {\n"
          "    int32 $b = 2;\n"
          "    echo $|\n"
          "}\n";
    const auto answer = complete(session, path, marked);

    REQUIRE(has(answer, "$a"));
    REQUIRE(has(answer, "$b"));
    REQUIRE_FALSE(has(answer, "$nope"));
    REQUIRE_FALSE(has(answer, "$global"));
    REQUIRE_FALSE(has(answer, "$this"));
    REQUIRE(answer.replace_start == marked.find("$|"));
    REQUIRE(item_of(answer, "$a")->label_description == "int32");
}

TEST_CASE("completion: $this and its members in a method", "[lsp]")
{
    const Compiler::DriverOptions driver = lsp_driver();
    Compiler::Lsp::Session session(driver);
    const std::filesystem::path path = "/tmp/lsp-complete-this.eco";
    session.did_open(path, 1, k_point);
    session.rebuild();

    std::string marked = k_point;
    marked.replace(marked.find("return $this->x"), std::string("return $this->x").size(), "return $th|");
    const auto variables = complete(session, path, marked);
    REQUIRE(has(variables, "$this"));
}

TEST_CASE("completion: enum cases after ::; payload fields stay off ->", "[lsp]")
{
    const Compiler::DriverOptions driver = lsp_driver();
    Compiler::Lsp::Session session(driver);
    const std::filesystem::path path = "/tmp/lsp-complete-enum.eco";
    const std::string source
        = "enum Distance\n"
          "{\n"
          "    case meter(int32 $v);\n"
          "    case mile(int32 $v);\n"
          "    function twice() : int32 { return 2; }\n"
          "}\n"
          "function main() : void {\n"
          "    Distance $d = Distance::meter(1);\n"
          "}\n";
    session.did_open(path, 1, source);
    session.rebuild();

    const auto statics = complete(session, path,
        "enum Distance\n"
        "{\n"
        "    case meter(int32 $v);\n"
        "    case mile(int32 $v);\n"
        "    function twice() : int32 { return 2; }\n"
        "}\n"
        "function main() : void {\n"
        "    Distance $d = Distance::|\n"
        "}\n");
    REQUIRE(has(statics, "meter"));
    REQUIRE(has(statics, "mile"));
    REQUIRE(item_of(statics, "meter")->insert_text == "meter($0)");
    REQUIRE(item_of(statics, "meter")->kind == Compiler::Lsp::CompletionItemKind::t_enum_member);
    const auto static_labels = labels(statics);
    REQUIRE(std::count(static_labels.begin(), static_labels.end(), "meter") == 1);

    const auto members = complete(session, path,
        "enum Distance\n"
        "{\n"
        "    case meter(int32 $v);\n"
        "    case mile(int32 $v);\n"
        "    function twice() : int32 { return 2; }\n"
        "}\n"
        "function main() : void {\n"
        "    Distance $d = Distance::meter(1);\n"
        "    $d->|\n"
        "}\n");
    REQUIRE(has(members, "twice"));
    REQUIRE_FALSE(has(members, "v"));
}

TEST_CASE("completion: shorthand offers the destination's cases", "[lsp]")
{
    const Compiler::DriverOptions driver = lsp_driver();
    Compiler::Lsp::Session session(driver);
    const std::filesystem::path path = "/tmp/lsp-complete-shorthand.eco";
    const std::string unit = "enum Unit { case meter; case mile; }\n";
    session.did_open(path, 1, unit + "function pick() : Unit { return Unit::meter; }\n");
    session.rebuild();

    const auto declared = complete(session, path,
        unit + "function pick() : Unit { return Unit::meter; }\n"
               "function main() : void { Unit $u = .| }\n", true);
    REQUIRE(has(declared, "meter"));
    REQUIRE(has(declared, "mile"));

    const auto returned = complete(session, path,
        unit + "function pick() : Unit { return .| }\n", true);
    REQUIRE(has(returned, "mile"));

    // a `.` that starts no shorthand answers nothing
    REQUIRE(complete(session, path, unit + "function main() : void { $x = 3.| }\n", true).items.empty());
}

TEST_CASE("completion: a namespace's functions and types after ::", "[lsp]")
{
    const Compiler::DriverOptions driver = lsp_driver();
    Compiler::Lsp::Session session(driver);
    const std::filesystem::path lib = "/tmp/lsp-complete-geo.eco";
    const std::filesystem::path app = "/tmp/lsp-complete-app.eco";
    session.did_open(lib, 1,
        "namespace geo;\n"
        "struct Shape { int32 $sides; }\n"
        "function area(int32 $w, int32 $h) : int32 { return $w * $h; }\n");
    session.did_open(app, 1, "function main() : void {\n}\n");
    session.rebuild();

    const auto answer = complete(session, app, "function main() : void {\n    echo geo::|\n}\n");
    REQUIRE(has(answer, "area"));
    REQUIRE(has(answer, "Shape"));

    const auto *area = item_of(answer, "area");
    REQUIRE(area->label_detail == "(int32 $w, int32 $h)");
    REQUIRE(area->label_description == "int32");

    const auto bare = complete(session, app, "function main() : void {\n    echo ge|\n}\n");
    REQUIRE(has(bare, "geo"));
}

TEST_CASE("completion: keywords depend on where the name is written", "[lsp]")
{
    const Compiler::DriverOptions driver = lsp_driver();
    Compiler::Lsp::Session session(driver);
    const std::filesystem::path path = "/tmp/lsp-complete-keywords.eco";
    session.did_open(path, 1, "function main() : void {\n}\n");
    session.rebuild();

    const auto statement = complete(session, path, "function main() : void {\n    fo|\n}\n");
    REQUIRE(has(statement, "foreach"));
    REQUIRE(has(statement, "for"));
    REQUIRE_FALSE(has(statement, "return"));

    const auto body = complete(session, path, "struct P {\n    co|\n}\nfunction main() : void {\n}\n");
    REQUIRE(has(body, "constructor"));
    REQUIRE(has(body, "const"));
    REQUIRE_FALSE(has(body, "continue"));

    const auto attribute = complete(session, path, "#[|\nfunction main() : void {\n}\n", true);
    REQUIRE(has(attribute, "inline"));
}

TEST_CASE("completion: a trigger character that starts nothing answers nothing", "[lsp]")
{
    const Compiler::DriverOptions driver = lsp_driver();
    Compiler::Lsp::Session session(driver);
    const std::filesystem::path path = "/tmp/lsp-complete-trigger.eco";
    session.did_open(path, 1, "function main() : void {\n}\n");
    session.rebuild();

    REQUIRE(complete(session, path, "function main() : void {\n    $a = 1 >| \n}\n", true).items.empty());
    REQUIRE(complete(session, path, "function main() : void {\n    // $|\n}\n", true).items.empty());
}

TEST_CASE("completion: without a snapshot the text still answers", "[lsp]")
{
    const std::string text = "function main() : void {\n    $count = 1;\n    $c\n}\n";
    const Compiler::Lsp::LiveText live(text);
    Compiler::Lsp::CompletionRequest request;
    request.live = &live;
    request.cursor = text.find("$c\n") + 2;

    const auto answer = Compiler::Lsp::completion(request);
    REQUIRE(has(answer, "$count"));
}

TEST_CASE("completion: members of an inline-array element", "[lsp]")
{
    const Compiler::DriverOptions driver = lsp_driver();
    Compiler::Lsp::Session session(driver);
    const std::filesystem::path path = "/tmp/lsp-complete-index-array.eco";
    const std::string source
        = k_point
        + "function main() : void {\n"
          "    Point[2] $ps;\n"
          "}\n";
    session.did_open(path, 1, source);
    session.rebuild();

    const auto answer = complete(session, path,
        k_point
        + "function main() : void {\n"
          "    Point[2] $ps;\n"
          "    $ps[0]->|\n"
          "}\n");

    REQUIRE(has(answer, "x"));
    REQUIRE(has(answer, "len"));
    REQUIRE_FALSE(has(answer, "secret"));
}

TEST_CASE("completion: members of a declared index operator", "[lsp]")
{
    const Compiler::DriverOptions driver = lsp_driver();
    Compiler::Lsp::Session session(driver);
    const std::filesystem::path path = "/tmp/lsp-complete-index-op.eco";
    const std::string source
        = k_point
        + "struct Slot { Point $a; Point $b; }\n"
          "operator (Slot& $s)[int32 $i] : Point& { if ($i == 0) { return $s->a; } return $s->b; }\n"
          "function main() : void {\n"
          "    Slot $s = Slot(Point(1, 2), Point(3, 4));\n"
          "}\n";
    session.did_open(path, 1, source);
    session.rebuild();

    const auto answer = complete(session, path,
        k_point
        + "struct Slot { Point $a; Point $b; }\n"
          "operator (Slot& $s)[int32 $i] : Point& { if ($i == 0) { return $s->a; } return $s->b; }\n"
          "function main() : void {\n"
          "    Slot $s = Slot(Point(1, 2), Point(3, 4));\n"
          "    $s[0]->|\n"
          "}\n");

    REQUIRE(has(answer, "x"));
    REQUIRE(has(answer, "len"));
}

TEST_CASE("completion: members through an optional chain", "[lsp]")
{
    const Compiler::DriverOptions driver = lsp_driver();
    Compiler::Lsp::Session session(driver);
    const std::filesystem::path path = "/tmp/lsp-complete-optional-chain.eco";
    const std::string source
        = k_point
        + "function main() : void {\n"
          "    Point? $maybe = null;\n"
          "}\n";
    session.did_open(path, 1, source);
    session.rebuild();

    const auto answer = complete(session, path,
        k_point
        + "function main() : void {\n"
          "    Point? $maybe = null;\n"
          "    $maybe?->|\n"
          "}\n");

    REQUIRE(has(answer, "x"));
    REQUIRE(has(answer, "len"));
}

TEST_CASE("completion: members of an imported function's result", "[lsp]")
{
    const Compiler::DriverOptions driver = lsp_driver();
    Compiler::Lsp::Session session(driver);
    const std::filesystem::path lib = "/tmp/lsp-complete-origin-lib.eco";
    const std::filesystem::path app = "/tmp/lsp-complete-origin-app.eco";
    session.did_open(lib, 1,
        "namespace geo;\n"
        "struct Point { int32 $x; int32 $y; function len() : int32 { return $this->x + $this->y; } }\n"
        "function origin() : Point { return Point(0, 0); }\n");
    session.did_open(app, 1, "use geo::origin;\nfunction main() : void {\n}\n");
    session.rebuild();

    const auto answer = complete(session, app,
        "use geo::origin;\n"
        "function main() : void {\n"
          "    $p = origin();\n"
          "    $p->|\n"
          "}\n");

    REQUIRE(has(answer, "x"));
    REQUIRE(has(answer, "len"));
}
