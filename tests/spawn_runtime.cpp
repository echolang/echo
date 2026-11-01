#include <catch2/catch_test_macros.hpp>

#include <eco_spawn.h>

#if defined(__linux__)
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>
#endif

#if defined(__linux__)
namespace
{
    typedef int (*SpawnFn)(int *, const char *, char *const[], char *const[], const char *, const int[3]);

    const SpawnFn k_spawns[] = { __eco_spawn_with_chdir, __eco_spawn_with_chdir_fallback };

    void spawn_marker_test(SpawnFn spawn, const int child_fd[3])
    {
        char dir[] = "/tmp/eco-spawn-XXXXXX";
        REQUIRE(mkdtemp(dir) != nullptr);

        char marker[80];
        REQUIRE(snprintf(marker, sizeof(marker), "%s/here", dir) < (int)sizeof(marker));
        FILE *f = fopen(marker, "w");
        REQUIRE(f != nullptr);
        fclose(f);

        int pid = 0;
        char *argv[] = {
            const_cast<char *>("/bin/sh"),
            const_cast<char *>("-c"),
            const_cast<char *>("test -f here"),
            nullptr
        };
        char *envp[] = { nullptr };

        const int rc = spawn(&pid, "/bin/sh", argv, envp, dir, child_fd);
        REQUIRE(rc == 0);
        REQUIRE(pid > 0);

        int status = 0;
        REQUIRE(waitpid(pid, &status, 0) == pid);
        REQUIRE(WIFEXITED(status));
        REQUIRE(WEXITSTATUS(status) == 0);

        unlink(marker);
        rmdir(dir);
    }

    std::string spawn_merged(SpawnFn spawn)
    {
        int fds[2];
        REQUIRE(pipe2(fds, O_CLOEXEC) == 0);

        int pid = 0;
        const int child_fd[3] = { ECO_SPAWN_INHERIT, fds[1], ECO_SPAWN_STDOUT };
        char *argv[] = {
            const_cast<char *>("/bin/sh"),
            const_cast<char *>("-c"),
            const_cast<char *>("echo out; echo err >&2"),
            nullptr
        };
        char *envp[] = { nullptr };

        const int rc = spawn(&pid, "/bin/sh", argv, envp, "/", child_fd);
        close(fds[1]);
        REQUIRE(rc == 0);
        REQUIRE(pid > 0);

        std::string out;
        char buf[256];
        ssize_t n = 0;
        while ((n = read(fds[0], buf, sizeof(buf))) > 0) {
            out.append(buf, static_cast<size_t>(n));
        }
        close(fds[0]);

        int status = 0;
        REQUIRE(waitpid(pid, &status, 0) == pid);
        REQUIRE(WIFEXITED(status));
        REQUIRE(WEXITSTATUS(status) == 0);

        return out;
    }
};
#endif

TEST_CASE("spawn with chdir runs the child in that directory", "[spawn]")
{
#if !defined(__linux__)
    int pid = 0;
    const int child_fd[3] = { ECO_SPAWN_INHERIT, ECO_SPAWN_INHERIT, ECO_SPAWN_INHERIT };
    REQUIRE(__eco_spawn_with_chdir(&pid, "", nullptr, nullptr, "", child_fd) == 38);
    REQUIRE(__eco_spawn_with_chdir_fallback(&pid, "", nullptr, nullptr, "", child_fd) == 38);
#else
    const int child_fd[3] = { ECO_SPAWN_INHERIT, ECO_SPAWN_INHERIT, ECO_SPAWN_INHERIT };
    for (SpawnFn spawn : k_spawns) {
        spawn_marker_test(spawn, child_fd);
    }
#endif
}

TEST_CASE("spawn with discarded stdio still chdirs", "[spawn]")
{
#if !defined(__linux__)
    SKIP("the helper is linux");
#else
    const int child_fd[3] = { ECO_SPAWN_DEVNULL, ECO_SPAWN_DEVNULL, ECO_SPAWN_DEVNULL };
    for (SpawnFn spawn : k_spawns) {
        spawn_marker_test(spawn, child_fd);
    }
#endif
}

TEST_CASE("spawn merges stderr into the stdout it just placed", "[spawn]")
{
#if !defined(__linux__)
    SKIP("the helper is linux");
#else
    for (SpawnFn spawn : k_spawns) {
        REQUIRE(spawn_merged(spawn) == "out\nerr\n");
    }
#endif
}

TEST_CASE("spawn with a null cwd keeps this process's directory", "[spawn]")
{
#if !defined(__linux__)
    SKIP("the helper is linux");
#else
    const int child_fd[3] = { ECO_SPAWN_INHERIT, ECO_SPAWN_INHERIT, ECO_SPAWN_INHERIT };
    char *argv[] = { const_cast<char *>("/bin/true"), nullptr };
    char *envp[] = { nullptr };

    for (SpawnFn spawn : k_spawns) {
        int pid = 0;
        const int rc = spawn(&pid, "/bin/true", argv, envp, nullptr, child_fd);
        REQUIRE(rc == 0);
        REQUIRE(pid > 0);

        int status = 0;
        REQUIRE(waitpid(pid, &status, 0) == pid);
        REQUIRE(WIFEXITED(status));
        REQUIRE(WEXITSTATUS(status) == 0);

        pid = 0;
        const int empty = spawn(&pid, "/bin/true", argv, envp, "", child_fd);
        REQUIRE(empty == 0);
        REQUIRE(pid > 0);
        REQUIRE(waitpid(pid, &status, 0) == pid);
        REQUIRE(WIFEXITED(status));
        REQUIRE(WEXITSTATUS(status) == 0);
    }
#endif
}

TEST_CASE("spawn into a missing directory returns errno and reaps the child", "[spawn]")
{
#if !defined(__linux__)
    SKIP("the helper is linux");
#else
    char dir[] = "/tmp/eco-spawn-XXXXXX";
    REQUIRE(mkdtemp(dir) != nullptr);
    REQUIRE(rmdir(dir) == 0);

    const int child_fd[3] = { ECO_SPAWN_INHERIT, ECO_SPAWN_INHERIT, ECO_SPAWN_INHERIT };
    char *argv[] = { const_cast<char *>("/bin/true"), nullptr };
    char *envp[] = { nullptr };

    for (SpawnFn spawn : k_spawns) {
        int pid = 0;
        const int rc = spawn(&pid, "/bin/true", argv, envp, dir, child_fd);
        REQUIRE(rc == ENOENT);

        // posix_spawn reaps on a failed addchdir; the fallback waitpids itself
        if (pid > 0) {
            int status = 0;
            REQUIRE(waitpid(pid, &status, WNOHANG) != pid);
        }
    }
#endif
}
