#ifndef TESTS_ECO_CHECK_DIRECTIVES_H
#define TESTS_ECO_CHECK_DIRECTIVES_H

#pragma once

#include <string>
#include <vector>

namespace EchoTests
{
    // one directive from a `.test` file's dump section.
    //
    // the format is LLVM's own FileCheck, reduced to the two directives that carry their weight,
    // plus a host-gated form so a lowering that differs per OS can be pinned without dropping
    // the assertion on every other machine:
    //
    //     CHECK:               <substring>   must appear at or after the previous CHECK's match
    //     CHECK-NOT:           <substring>   must not appear between the surrounding CHECKs
    //     CHECK-<os>:          <substring>   CHECK, only when the suite's host is <os>
    //     CHECK-NOT-<os>:      <substring>   CHECK-NOT, only when the suite's host is <os>
    //
    // `<os>` is one of TargetFacts' operating systems, written uppercase in the directive
    // (`CHECK-WINDOWS:`) and stored lowercase (`windows`). empty `host_os` means every host.
    // a gated directive is skipped on the other machines: it does not advance the cursor and
    // it does not search. `--target-os` does not select these - that flag picks `#[if:]` arms,
    // not the CRT names the object is emitted with
    //
    // `line` is the line in the `.test` file rather than in the section, so a failure names a place
    // the author can open
    struct CheckDirective
    {
        bool negated = false;
        std::string text;
        size_t line = 0;
        std::string host_os;
    };

    // parses a directive section body. `first_line` is the `.test` line the body starts on and
    // `origin` is what a message names the file as.
    //
    // false with a `file:line: message` in `out_error` on a line that is neither blank, a comment
    // (`#` / `//`), nor a directive - a mistyped `CHEK:` that silently checks nothing is the one
    // failure mode this format exists to refuse. an unknown `CHECK-FOO:` is the same class of error:
    // `<os>` has to be a TargetFacts operating system, not a silently-skipped word. an empty
    // section is the caller's to reject, since only the caller knows which section it was
    bool parse_check_directives(
        const std::string &body,
        size_t first_line,
        const std::string &origin,
        std::vector<CheckDirective> &out_directives,
        std::string &out_error
    );

    // applies the directives to `haystack`, "" on success or the failure message.
    //
    // a positive CHECK advances a cursor, so ordering is part of the assertion and each CHECK can
    // only match at or after the last one. that is what gives free function scoping - a
    // `CHECK: define i32 @main(` followed by CHECKs that can then only match below it - and it is
    // why a CHECK-NOT is scoped to the region *between* its neighbours rather than to the whole
    // text. whole-text would be useless here: `mem::` declares `@malloc` and the class runtime
    // calls it, both above `main`, so "this function does not allocate" has to mean "not in this
    // region"
    std::string apply_check_directives(const std::vector<CheckDirective> &directives, const std::string &haystack);

    // same, against an explicit host (`windows` / `linux` / `darwin`) so a case can be judged
    // without asking the machine it happens to be running on
    std::string apply_check_directives(
        const std::vector<CheckDirective> &directives,
        const std::string &haystack,
        const std::string &host_os
    );

    // trims surrounding whitespace, including a trailing `\r` from an editor that writes CRLF
    std::string trim_whitespace(const std::string &s);

    // the one spelling of a located diagnostic, `file:line: message`. shared by both halves of the
    // `.test` reader - the format parser and the directive parser both report against the same file,
    // and an author who sees two shapes of location cannot tell they came from one reader
    std::string locate(const std::string &origin, size_t line, const std::string &message);
};

#endif
