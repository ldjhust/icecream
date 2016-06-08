/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/* vim: set ts=4 sw=4 et tw=99:  */
/*
 * distcc -- A simple distributed compiler system
 *
 * Copyright (C) 2002, 2003 by Martin Pool <mbp@samba.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */


#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#include <sys/types.h>
#include <sys/stat.h>

#include "client.h"

using namespace std;

// Whether any option controlling color output has been explicitly given.
bool explicit_color_diagnostics;

// Whether -fno-diagnostics-show-caret was given.
bool explicit_no_show_caret;

#define CLIENT_DEBUG 0

inline bool str_equal(const char* a, const char* b)
{
    return strcmp(a, b) == 0;
}

inline int str_startswith(const char *head, const char *worm)
{
    return !strncmp(head, worm, strlen(head));
}

static bool analyze_program(const char *name, CompileJob &job)
{
    string compiler_name = find_basename(name);

    string::size_type pos = compiler_name.rfind('/');

    if (pos != string::npos) {
        compiler_name = compiler_name.substr(pos);
    }

    job.setCompilerName(compiler_name);

    string suffix = compiler_name;

    if (compiler_name.size() > 2) {
        suffix = compiler_name.substr(compiler_name.size() - 2);
    }

    if ((suffix == "++") || (suffix == "CC")) {
        job.setLanguage(CompileJob::Lang_CXX);
    } else if (suffix == "cc") {
        job.setLanguage(CompileJob::Lang_C);
    } else if (compiler_name == "clang") {
        job.setLanguage(CompileJob::Lang_C);
    } else {
        job.setLanguage(CompileJob::Lang_Custom);
        log_info() << "custom command, running locally." << endl;
        return true;
    }

    return false;
}

bool analyse_argv(const char * const *argv, CompileJob &job, bool icerun, list<string> *extrafiles)
{
    ArgumentsList args;
    string ofile;
    string dwofile;

#if CLIENT_DEBUG > 1
    trace() << "scanning arguments ";

    for (int index = 0; argv[index]; index++) {
        trace() << argv[index] << " ";
    }

    trace() << endl;
#endif

    bool had_cc = (job.compilerName().size() > 0);
    bool always_local = analyze_program(had_cc ? job.compilerName().c_str() : argv[0], job);
    bool seen_c = false;
    bool seen_s = false;
    bool seen_mf = false;
    bool seen_md = false;
    bool seen_split_dwarf = false;
    // if rewriting includes and precompiling on remote machine, then cpp args are not local
    Argument_Type Arg_Cpp = compiler_only_rewrite_includes(job) ? Arg_Rest : Arg_Local;

    explicit_color_diagnostics = false;
    explicit_no_show_caret = false;

    if (icerun) {
        always_local = true;
        job.setLanguage(CompileJob::Lang_Custom);
        log_info() << "icerun, running locally." << endl;
    }

    bool is_wl_start = true;
    bool is_linker_flag = false;
    string wl_arg;
    string my_iFile;

    for (int i = had_cc ? 2 : 1; argv[i]; i++) {
        const char *a = argv[i];

        if (icerun) {
            args.append(a, Arg_Local);
        } else if (a[0] == '-') {
            if (strcmp(a, "-c") == 0) {
                if ( is_linker_flag ) {
                    // 这是最后一个参数了，不会再有Xlinker，将wl_arg添加到args里面去，链接参数是本地的
                    trace() << "添加进去了" << endl;
                    args.append( wl_arg, Arg_Local);
                    is_linker_flag = false;
                }
                else {
                    my_iFile = argv[i+1]; // -c 下一个参数就是待编译文件
                    trace() << "找到了input files：" << my_iFile << endl;
                }
            }

            if (!strcmp(a, "-E")) {
                always_local = true;
                args.append(a, Arg_Local);
                log_info() << "preprocessing, building locally" << endl;
            } else if (!strncmp(a, "-fdump", 6) || !strcmp(a, "-combine")) {
                always_local = true;
                args.append(a, Arg_Local);
                log_info() << "argument " << a << ", building locally" << endl;
            } else if (!strcmp(a, "-MD") || !strcmp(a, "-MMD")) {
                seen_md = true;
                args.append(a, Arg_Local);
                /* These two generate dependencies as a side effect.  They
                 * should work with the way we call cpp. */
            } else if (!strcmp(a, "-MG") || !strcmp(a, "-MP")) {
                args.append(a, Arg_Local);
                /* These just modify the behaviour of other -M* options and do
                 * nothing by themselves. */
            } else if (!strcmp(a, "-MF")) {
                seen_mf = true;
                args.append(a, Arg_Local);
                args.append(argv[++i], Arg_Local);
                /* as above but with extra argument */
            } else if (!strcmp(a, "-MT") || !strcmp(a, "-MQ")) {
                args.append(a, Arg_Local);
                args.append(argv[++i], Arg_Local);
                /* as above but with extra argument */
            } else if (a[1] == 'M') {
                /* -M(anything else) causes the preprocessor to
                   produce a list of make-style dependencies on
                   header files, either to stdout or to a local file.
                   It implies -E, so only the preprocessor is run,
                   not the compiler.  There would be no point trying
                   to distribute it even if we could. */
                always_local = true;
                args.append(a, Arg_Local);
                log_info() << "argument " << a << ", building locally" << endl;
            } else if (str_equal("--param", a)) {
                args.append(a, Arg_Remote);

                /* skip next word, being option argument */
                if (argv[i + 1]) {
                    args.append(argv[++i], Arg_Remote);
                }
            } else if (a[1] == 'B') {
                /* -B overwrites the path where the compiler finds the assembler.
                   As we don't use that, better force local job.
                */
                always_local = true;
                args.append(a, Arg_Local);
                log_info() << "argument " << a << ", building locally" << endl;

                if (str_equal(a, "-B")) {
                    /* skip next word, being option argument */
                    if (argv[i + 1]) {
                        args.append(argv[++i], Arg_Local);
                    }
                }
            } else if (str_startswith("-Wa,", a)) {
                /* Options passed through to the assembler.  The only one we
                 * need to handle so far is -al=output, which directs the
                 * listing to the named file and cannot be remote.  There are
                 * some other options which also refer to local files,
                 * but most of them make no sense when called via the compiler,
                 * hence we only look for -a[a-z]*= and localize the job if we
                 * find it.
                 */
                const char *pos = a;
                bool local = false;

                while ((pos = strstr(pos + 1, "-a"))) {
                    pos += 2;

                    while ((*pos >= 'a') && (*pos <= 'z')) {
                        pos++;
                    }

                    if (*pos == '=') {
                        local = true;
                        break;
                    }

                    if (!*pos) {
                        break;
                    }
                }

                /* Some weird build systems pass directly additional assembler files.
                 * Example: -Wa,src/code16gcc.s
                 * Need to handle it locally then. Search if the first part after -Wa, does not start with -
                 */
                pos = a + 3;

                while (*pos) {
                    if ((*pos == ',') || (*pos == ' ')) {
                        pos++;
                        continue;
                    }

                    if (*pos == '-') {
                        break;
                    }

                    local = true;
                    break;
                }

                if (local) {
                    always_local = true;
                    args.append(a, Arg_Local);
                    log_info() << "argument " << a << ", building locally" << endl;
                } else {
                    args.append(a, Arg_Remote);
                }
            } else if (!strcmp(a, "-S")) {
                seen_s = true;
            } else if (!strcmp(a, "-fprofile-arcs")
                       || !strcmp(a, "-ftest-coverage")
                       || !strcmp(a, "-frepo")
                       || !strcmp(a, "-fprofile-generate")
                       || !strcmp(a, "-fprofile-use")
                       || !strcmp(a, "-save-temps")
                       || !strcmp(a, "--save-temps")
                       || !strcmp(a, "-fbranch-probabilities")) {
                log_info() << "compiler will emit profile info (argument " << a << "); building locally" << endl;
                always_local = true;
                args.append(a, Arg_Local);
            } else if (!strcmp(a, "-gsplit-dwarf")) {
                seen_split_dwarf = true;
            } else if ( strcmp( a, "-Xlinker" ) == 0 ) { // here
                trace() << "哈哈，找到了Xlinker 替换为Wl哈哈哈哈哈哈哈哈哈哈哈 " << endl;
                if ( is_wl_start ) {
                    is_wl_start = false;
                    wl_arg.append( "-Wl" );
                }

                wl_arg.append( "," );
                wl_arg.append( argv[++i] );
                trace() << "哦哦哦，现在wl_arg是：" << wl_arg << endl;

                is_linker_flag = true;
                continue;
            } else if ( strcmp( a, "--serialize-diagnostics") == 0
                        || strcmp( a, "-iquote") == 0
                        || strcmp( a, "-MT") == 0
                        || strcmp( a, "-MF") == 0) {
                // 这个是依赖于本机的日志文件，会让icecc以为有两个输入文件导致只在本地编译，我们直接跳过忽略
                ++i;
                trace() << "忽略这个参数" << a << endl;
                continue;
            } else if ( strcmp( a, "-fmodules-validate-once-per-build-session") == 0
                        || strcmp( a, "-MMD") == 0
                        || strncmp( a, "-fbuild-session-file", 20) == 0
                        || strncmp( a, "-I", 2) == 0
                        || strncmp( a, "-F", 2) == 0) {
                trace() << "忽略" << a << endl;
                continue;
            } else if (str_equal(a, "-x")) {
                args.append(a, Arg_Rest);
                bool unsupported = true;
                if (const char *opt = argv[i + 1]) {
                    ++i;
                    args.append(opt, Arg_Rest);
                    if (str_equal(opt, "c++")
                        || str_equal(opt, "c")
                        || str_equal(opt, "objective-c")
                        || str_equal(opt, "objective-c++")) {
                        CompileJob::Language lang = str_equal(opt, "c++") ? CompileJob::Lang_CXX : (str_equal(opt, "c") ? CompileJob::Lang_C : CompileJob::Lang_OBJC);
                        job.setLanguage(lang); // will cause -x used remotely twice, but shouldn't be a problem
                        unsupported = false;
                    }
                }
                if (unsupported) {
                    log_info() << "unsupported -x option; running locally" << endl;
                    always_local = true;
                }
            } else if (!strcmp(a, "-march=native") || !strcmp(a, "-mcpu=native")
                       || !strcmp(a, "-mtune=native")) {
                log_info() << "-{march,mpcu,mtune}=native optimizes for local machine, "
                           << "building locally"
                           << endl;
                always_local = true;
                args.append(a, Arg_Local);

            } else if (!strcmp(a, "-fexec-charset") || !strcmp(a, "-fwide-exec-charset") || !strcmp(a, "-finput-charset") ) {
#if CLIENT_DEBUG
                log_info() << "-f*-charset assumes charset conversion in the build environment; must be local" << endl;
#endif
                always_local = true;
                args.append(a, Arg_Local);
            } else if (!strcmp(a, "-c")) {
                seen_c = true;
            } else if (str_startswith("-o", a)) {
                if (!strcmp(a, "-o")) {
                    /* Whatever follows must be the output */
                    if (argv[i + 1]) {
                        ofile = argv[++i];
                    }
                } else {
                    a += 2;
                    ofile = a;
                }

                if (ofile == "-") {
                    /* Different compilers may treat "-o -" as either "write to
                     * stdout", or "write to a file called '-'".  We can't know,
                     * so we just always run it locally.  Hopefully this is a
                     * pretty rare case. */
                    log_info() << "output to stdout?  running locally" << endl;
                    always_local = true;
                }
            } else if (str_equal("-include", a)) {
                /* This has a duplicate meaning. it can either include a file
                   for preprocessing or a precompiled header. decide which one.  */

                if (argv[i + 1]) {
                    ++i;
                    std::string p = argv[i];
                    string::size_type dot_index = p.find_last_of('.');

                    if (dot_index != string::npos) {
                        string ext = p.substr(dot_index + 1);

                        if (ext[0] != 'h' && ext[0] != 'H'
                            && (access(p.c_str(), R_OK)
                                || access((p + ".gch").c_str(), R_OK))) {
                            log_info() << "include file or gch file for argument " << a << " " << p
                                       << " missing, building locally" << endl;
                            always_local = true;
                        }
                    } else {
                        log_info() << "argument " << a << " " << p << ", building locally" << endl;
                        always_local = true;    /* Included file is not header.suffix or header.suffix.gch! */
                    }

                    args.append(a, Arg_Local);
                    args.append(argv[i], Arg_Local);
                }
            } else if (str_equal("-include-pch", a)) {
                /* Clang's precompiled header, it's probably not worth it sending the PCH file. */
                if (argv[i + 1]) {
                    ++i;
                }

                always_local = true;
                log_info() << "argument " << a << ", building locally" << endl;
            } else if (str_equal("-D", a) || str_equal("-U", a)) {
                args.append(a, Arg_Cpp);

                /* skip next word, being option argument */
                if (argv[i + 1]) {
                    ++i;
                    args.append(argv[i], Arg_Cpp);
                }
            } else if (str_equal("-I", a)
                       || str_equal("-L", a)
                       || str_equal("-l", a)
                       || str_equal("-MF", a)
                       || str_equal("-MT", a)
                       || str_equal("-MQ", a)
                       || str_equal("-imacros", a)
                       || str_equal("-iprefix", a)
                       || str_equal("-iwithprefix", a)
                       || str_equal("-isystem", a)
                       || str_equal("-iquote", a)
                       || str_equal("-imultilib", a)
                       // || str_equal("-isysroot", a)
                       || str_equal("-iwithprefixbefore", a)
                       || str_equal("-idirafter", a)) {
                args.append(a, Arg_Local);

                /* skip next word, being option argument */
                if (argv[i + 1]) {
                    ++i;

                    if (str_startswith("-O", argv[i])) {
                        always_local = true;
                        log_info() << "argument " << a << " " << argv[i] << ", building locally" << endl;
                    }

                    args.append(argv[i], Arg_Local);
                }
            } else if (str_startswith("-Wp,", a)
                       || str_startswith("-D", a)
                       || str_startswith("-U", a)) {
                args.append(a, Arg_Cpp);
            } else if (str_startswith("-I", a)
                       || str_startswith("-l", a)
                       || str_startswith("-L", a)) {
                args.append(a, Arg_Local);
            } else if (str_equal("-undef", a)) {
                args.append(a, Arg_Cpp);
            } else if (str_equal("-nostdinc", a)
                       || str_equal("-nostdinc++", a)
                       || str_equal("-MD", a)
                       || str_equal("-MMD", a)
                       || str_equal("-MG", a)
                       || str_equal("-MP", a)) {
                args.append(a, Arg_Local);
            } else if (str_equal("-fno-color-diagnostics", a)) {
                explicit_color_diagnostics = true;
                args.append(a, Arg_Rest);
            } else if (str_equal("-fcolor-diagnostics", a)) {
                explicit_color_diagnostics = true;
                args.append(a, Arg_Rest);
            } else if (str_equal("-fno-diagnostics-color", a)
                       || str_equal("-fdiagnostics-color=never", a)) {
                explicit_color_diagnostics = true;
                args.append(a, Arg_Rest);
            } else if (str_equal("-fdiagnostics-color", a)
                       || str_equal("-fdiagnostics-color=always", a)) {
                explicit_color_diagnostics = true;
                args.append(a, Arg_Rest);
            } else if (str_equal("-fdiagnostics-color=auto", a)) {
                // Drop the option here and pretend it wasn't given,
                // the code below will decide whether to enable colors or not.
                explicit_color_diagnostics = false;
            } else if (str_equal("-fno-diagnostics-show-caret", a)) {
                explicit_no_show_caret = true;
                args.append(a, Arg_Rest);
            } else if (str_equal("-flto", a)) {
                // pointless when preprocessing, and Clang would emit a warning
                args.append(a, Arg_Remote);
            } else if (str_startswith("-fplugin=", a)) {
                string file = a + strlen("-fplugin=");

                if (access(file.c_str(), R_OK) == 0) {
                    file = get_absfilename(file);
                    extrafiles->push_back(file);
                } else {
                    always_local = true;
                    log_info() << "plugin for argument " << a << " missing, building locally" << endl;
                }

                args.append("-fplugin=" + file, Arg_Rest);
            } else if (str_equal("-Xclang", a)) {
                if (argv[i + 1]) {
                    ++i;
                    const char *p = argv[i];

                    if (str_equal("-load", p)) {
                        if (argv[i + 1] && argv[i + 2] && str_equal(argv[i + 1], "-Xclang")) {
                            args.append(a, Arg_Rest);
                            args.append(p, Arg_Rest);
                            string file = argv[i + 2];

                            if (access(file.c_str(), R_OK) == 0) {
                                file = get_absfilename(file);
                                extrafiles->push_back(file);
                            } else {
                                always_local = true;
                                log_info() << "plugin for argument "
                                           << a << " " << p << " " << argv[i + 1] << " " << file
                                           << " missing, building locally" << endl;
                            }

                            args.append(argv[i + 1], Arg_Rest);
                            args.append(file, Arg_Rest);
                            i += 2;
                        }
                    } else {
                        args.append(a, Arg_Rest);
                        args.append(p, Arg_Rest);
                    }
                }
            } else {
                args.append(a, Arg_Rest);
            }
        } else if (a[0] == '@') {
            args.append(a, Arg_Local);
        } else {
            args.append(a, Arg_Rest);
        }
    }

    if (!seen_c && !seen_s) {
        if (!always_local) {
            log_info() << "neither -c nor -S argument, building locally" << endl;
        }
        always_local = true;
    } else if (seen_s) {
        if (seen_c) {
            log_info() << "can't have both -c and -S, ignoring -c" << endl;
        }

        args.append("-S", Arg_Remote);
    } else {
        args.append("-c", Arg_Remote);
        if (seen_split_dwarf) {
            job.setDwarfFissionEnabled(true);
        }
    }

    if (!always_local) {
        trace() << "本地编译吗：" << always_local << endl;
        ArgumentsList backup = args;

        /* TODO: ccache has the heuristic of ignoring arguments that are not
         * extant files when looking for the input file; that's possibly
         * worthwile.  Of course we can't do that on the server. */
        string ifile;

        for (ArgumentsList::iterator it = args.begin(); it != args.end();) {
            if (it->first == "-") {
                always_local = true;
                log_info() << "stdin/stdout argument, building locally" << endl;
                break;
            }

            // Skip compiler arguments which are followed by another
            // argument not starting with -.
//             if (it->first == "-Xclang" || it->first == "-x" || it->first == "-isysroot") {
//                 ++it;
//                 ++it;
//             } else if (it->second != Arg_Rest || it->first.at(0) == '-'
//                        || it->first.at(0) == '@') {
//                 ++it;
//             } else if (ifile.empty()) {
// #if CLIENT_DEBUG
//                 log_info() << "input file: " << it->first << endl;
// #endif
//                 job.setInputFile(it->first);
//                 ifile = it->first;
//                 it = args.erase(it);
//             } else {
//                 log_info() << "found another non option on command line. Two input files? "
//                            << it->first << endl;
//                 trace() << "因为可能有两个input files，所以只能在本地运行" << endl;
//                 always_local = true;
//                 args = backup;
//                 job.setInputFile(string());
//                 break;
//             }
            // 你只是要找input file，我告诉你
            if ((it->first).compare( my_iFile ) == 0) {
                job.setInputFile(it->first);
                ifile = it->first;
                trace() << "待编译文件是: " << it->first << endl;
                it = args.erase(it);
                break;
            }

            ++it;
        }

        trace() << "现在本地编译：" << always_local << endl;

        if (ifile.find('.') != string::npos) {
            string::size_type dot_index = ifile.find_last_of('.');
            string ext = ifile.substr(dot_index + 1);

            if (ext == "cc"
                || ext == "cpp" || ext == "cxx"
                || ext == "cp" || ext == "c++"
                || ext == "C" || ext == "ii") {
#if CLIENT_DEBUG

                if (job.language() != CompileJob::Lang_CXX) {
                    log_info() << "switching to C++ for " << ifile << endl;
                }

#endif
                job.setLanguage(CompileJob::Lang_CXX);
            } else if (ext == "mi" || ext == "m"
                       || ext == "mii" || ext == "mm"
                       || ext == "M") {
                job.setLanguage(CompileJob::Lang_OBJC);
                trace() << "带编译Objective" << endl;
            } else if (ext == "s" || ext == "S" // assembler
                       || ext == "ads" || ext == "adb" // ada
                       || ext == "f" || ext == "for" // fortran
                       || ext == "FOR" || ext == "F"
                       || ext == "fpp" || ext == "FPP"
                       || ext == "r")  {
                always_local = true;
                log_info() << "source file " << ifile << ", building locally" << endl;
            } else if (ext != "c" && ext != "i") {   // C is special, it depends on arg[0] name
                log_warning() << "unknown extension " << ext << endl;
                always_local = true;
            }

            if (!always_local && ofile.empty()) {
                ofile = ifile.substr(0, dot_index);

                if (seen_s) {
                    ofile += ".s";
                } else {
                    ofile += ".o";
                }

                string::size_type slash = ofile.find_last_of('/');

                if (slash != string::npos) {
                    ofile = ofile.substr(slash + 1);
                }
            }

            if (!always_local && seen_md && !seen_mf) {
                string dfile = ofile.substr(0, ofile.find_last_of('.')) + ".d";

#if CLIENT_DEBUG
                log_info() << "dep file: " << dfile << endl;
#endif

                args.append("-MF", Arg_Local);
                args.append(dfile, Arg_Local);
            }
        }

    } else {
        job.setInputFile(string());
    }

    struct stat st;

    if (ofile.empty() || (!stat(ofile.c_str(), &st) && !S_ISREG(st.st_mode))) {
        if (!always_local) {
            log_info() << "output file empty or not a regular file, building locally" << endl;
        }
        always_local = true;
    }

    trace() << "ofile这里always_local: " << always_local << endl;

    // redirecting compiler's output will turn off its automatic coloring, so force it
    // when it would be used, unless explicitly set
    if (compiler_has_color_output(job) && !explicit_color_diagnostics) {
        if (compiler_is_clang(job))
            args.append("-fcolor-diagnostics", Arg_Rest);
        else
            args.append("-fdiagnostics-color", Arg_Rest); // GCC
    }

    job.setFlags(args);
    job.setOutputFile(ofile);

    trace() << "scanned result: local args=" << concat_args(job.localFlags())
            << ", remote args=" << concat_args(job.remoteFlags())
            << ", rest=" << concat_args(job.restFlags())
            << ", local=" << always_local
            << ", compiler=" << job.compilerName()
            << ", lang=" << job.language()
            << endl;
    trace() << "最终确定的是否本地编译: " << always_local << endl;
    trace() << "最终确定的input file是：" << job.inputFile() << endl;
    trace() << "最终确定的output file是：" << job.outputFile() << endl;

    return always_local;
}
