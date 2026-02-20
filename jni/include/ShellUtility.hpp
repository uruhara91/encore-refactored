/*
 * Copyright (C) 2024-2026 Rem01Gaming
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <cstdio>
#include <memory>
#include <vector>
#include <cstdarg>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

struct PipeResult {
    FILE* stream;
    pid_t pid;

    PipeResult(FILE* s, pid_t p) : stream(s), pid(p) {}

    void close() {
        if (stream) {
            fclose(stream);
            stream = nullptr;
        }
        if (pid > 0) {
            // OPTIMASI: Pastikan proses anak mati sebelum di-wait agar tidak blocking!
            kill(pid, SIGKILL); 
            waitpid(pid, nullptr, 0);
            pid = -1;
        }
    }

    ~PipeResult() { close(); }
    
    PipeResult(const PipeResult&) = delete;
    PipeResult& operator=(const PipeResult&) = delete;
    
    PipeResult(PipeResult&& other) noexcept : stream(other.stream), pid(other.pid) {
        other.stream = nullptr;
        other.pid = -1;
    }
};

inline PipeResult popen_direct(const std::vector<std::string> &args) {
    int pipefd[2];
    
    // OPTIMASI: Gunakan pipe2 dengan O_CLOEXEC agar child tidak mewarisi fd ini sembarangan
    if (pipe2(pipefd, O_CLOEXEC) == -1) return PipeResult(nullptr, -1);
    
    pid_t pid = fork();
    if (pid == -1) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return PipeResult(nullptr, -1);
    }

    if (pid == 0) { // Child
        // OPTIMASI: Bungkam STDERR agar log daemon Anda bersih jika command gagal
        int devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            ::close(devnull);
        }

        ::close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO); // STDOUT masuk ke pipe. (dup2 menghapus O_CLOEXEC, ini yang kita mau)
        ::close(pipefd[1]);

        std::vector<char *> cargs;
        for (const auto &arg : args) {
            cargs.push_back(const_cast<char *>(arg.c_str()));
        }
        cargs.push_back(nullptr);

        execvp(cargs[0], cargs.data());
        _exit(127);
    } 

    // Parent
    ::close(pipefd[1]);
    return PipeResult(fdopen(pipefd[0], "r"), pid);
}

inline int systemv(const char *format, ...) {
    char command[512];
    va_list args;
    va_start(args, format);
    vsnprintf(command, sizeof(command), format, args);
    va_end(args);
    return system(command);
}