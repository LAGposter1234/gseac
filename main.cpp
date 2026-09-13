#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <sstream>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <stdexcept>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <filesystem>

std::string read_file(const std::string &filename) {
    std::ifstream file(filename);
    if (!file)
        throw std::runtime_error("failed to open " + filename);

    return std::string(
        std::istreambuf_iterator<char>(file),
                       std::istreambuf_iterator<char>()
    );
}

int exec(const char *path, char *const argv[]) {
    pid_t pid = fork();

    if (pid < 0)
        return -1;

    if (pid == 0) {
        execve(path, argv, environ);
        _exit(127);
    }

    int status;
    waitpid(pid, &status, 0);

    return status;
}

std::string make_temp_dir() {
    char path[] = "/tmp/gseac-XXXXXX";

    if (!mkdtemp(path))
        throw std::runtime_error("failed to create temporary directory");

    return path;
}

static std::string src;

std::ostringstream data;
std::ostringstream text;
std::ofstream out;

void emit_line(const std::string &line) {
    out << line << '\n';
}

void emit_text(const std::string &line) {
    text << line << '\n';
}

void emit_data(const std::string &line) {
    data << line << '\n';
}

uint64_t line_count(const std::string &src) {
    if (src.empty())
        return 0;

    uint64_t count = 1;

    for (char c : src) {
        if (c == '\n')
            count++;
    }

    return count;
}

std::vector<std::string> split_lines(const std::string &src) {
    std::vector<std::string> lines;
    std::stringstream ss(src);
    std::string line;

    while (std::getline(ss, line))
        lines.push_back(line);

    return lines;
}

std::vector<std::vector<std::string>> split_words(const std::string &src) {
    std::vector<std::vector<std::string>> program;

    for (const std::string &line : split_lines(src)) {
        std::vector<std::string> words;
        std::string word;
        bool quote = false;

        for (char c : line) {
            if (c == '\'' || c == '"') {
                quote = !quote;
                word += c;
            } else if (std::isspace((unsigned char)c) && !quote) {
                if (!word.empty()) {
                    words.push_back(word);
                    word.clear();
                }
            } else {
                word += c;
            }
        }

        if (!word.empty())
            words.push_back(word);

        program.push_back(words);
    }

    return program;
}

void unimplemented(int lineno, const std::vector<std::string> &line) {
    std::cout << "line " << lineno
    << ": \033[1;31merror\033[0m: "
    << "\033[1;37msorry, unimplemented\033[0m: ";

    for (const auto &word : line)
        std::cout << word << ' ';

    std::cout << '\n';
    _exit(1);
}

void error(int lineno, const std::vector<std::string> &line, std::string reason) {
    std::cout << "line " << lineno
    << ": \033[1;31merror\033[0m: "
    << reason << " :";

    for (const auto &word : line)
        std::cout << word << ' ';

    std::cout << '\n';
    _exit(1);
}

void warning(int lineno, const std::vector<std::string> &line, std::string reason) {
    std::cout << "line " << lineno
    << ": \033[1;38mwarning\033[0m: "
    << reason << " :";

    for (const auto &word : line)
        std::cout << word << ' ';

    std::cout << '\n';
}

class Object {
public:
    std::string name;
    std::string type;
    bool global;
    std::string value;
};

class Function {
public:
    std::string name;
    std::string type;
    bool global;
    std::vector<std::vector<std::string>> body;
};

class Module {
public:
    std::vector<Function> functions;
    std::vector<Object> data;
};

const Object *find_object(const Module &module, const std::string &name) {
    for (const auto &object : module.data)
        if (object.name == name)
            return &object;

    return nullptr;
}

Module make_module(const std::string src);

void copy_modules(Module& dest, std::string src) {
    Module module = make_module(read_file(src));

    for (const auto &function : module.functions)
        if (function.global)
            dest.functions.push_back(function);

    for (const auto &object : module.data)
        if (object.global)
            dest.data.push_back(object);
}

std::string type_to_nasm_type(const std::string &type) {
    if (type == "db") return "byte";
    if (type == "dw") return "word";
    if (type == "dd") return "qword";
    if (type == "dq") return "qword";
    return "";
}


Module make_module(const std::string src) {
    std::vector<std::vector<std::string>> program = split_words(src);
    Module current;

    for (int lineno = 0; lineno < program.size(); lineno++) {
        const auto &line = program[lineno];
        if (line.empty())
            continue;

        if (line[0] == "using") {
            std::string path = line[1];

            if (path.front() == '"')
                path = "./" + path.substr(1, path.size() - 2);
            else if (path.front() == '<')
                path = "/usr/include/sea/" + path.substr(1, path.size() - 2);

            copy_modules(current, path);
        } else if (line[0] == "global" || line[0] == "private") {
            if (line.size() < 2) {
                error(lineno, line, "Invalid function definition");
            }

            Function function;
            function.global = line[0] == "global";
            function.name = line[1];

            while (++lineno < program.size()) {
                if (program[lineno].size() == 1 && program[lineno][0] == "end")
                    break;

                function.body.push_back(program[lineno]);
            }

            current.functions.push_back(function);
        } else if (line[0] == "let") {
            // let global int x = 0
            if (line.size() < 6) {
                error(lineno, line, "Invalid variable definition");
            }

            Object obj;
            obj.global = line[1] == "global";
            obj.name = line[3];
            if (line[4] != "=" && line[4] != "be") error(lineno, line, "Invalid variable definition");
            if (line[2] == "int") {
                obj.type = "dd";
            } else if (line[2] == "short") {
                obj.type = "dw";
            } else if (line[2] == "byte" || line[2] == "string") {
                obj.type = "db";
            } else if (line[2] == "long") {
                obj.type = "dq";
            } else {
                error(lineno, line, line[4] + " is not a valid type");
            }
            obj.value = line[5];
            current.data.push_back(obj);
        }
    }

    return current;
}

static int generated_names = 0;

std::string gen_name(std::string midfix) {
    return "_" + midfix + std::to_string(generated_names++);
}

std::string gen_local_name(std::string midfix) {
    return ".L" + midfix + std::to_string(generated_names++);
}

bool is_number(const std::string &str) {
    if (str.empty())
        return false;

    for (char c : str)
        if (!std::isdigit((unsigned char)c))
            return false;

    return true;
}

bool is_object(const Module &module, const std::string &name) {
    for (const auto &object : module.data)
        if (object.name == name)
            return true;

    return false;
}

std::string escape_string(const std::string &str) {
    std::string out;

    for (size_t i = 0; i < str.size(); i++) {
        if (str[i] == '\\' && i + 1 < str.size()) {
            if (str[i + 1] == 'n') {
                out += "\", 10, \"";
                i++;
                continue;
            }
        }

        out += str[i];
    }

    return out;
}



void compile_function(const Function &function, const Module &module) {
    if (function.global)
        emit_line("global " + function.name);

    emit_text(function.name + ":");

    int lineno = 0;
    for (const auto &line : function.body) {
        if (line.empty())
            continue;

        if(line[0] == "return") {
            if (line.size() < 2) {
                emit_text("ret");
            } else if (line[1][0] == '"') {
                std::string name = gen_name("str");
                std::string str = line[1].substr(1, line[1].size() - 2);
                emit_data(name + ": db \"" + escape_string(str) + "\", 0");
                emit_text("mov eax, " + name);
                emit_text("ret");
            } else {
                if (is_number(line[1])) {
                    emit_text("mov eax, " + line[1]);
                    emit_text("ret");
                } else if (is_object(module, line[1])) {
                    for (const auto &object : module.data) {
                        if (object.name != line[1])
                            continue;

                        std::string type = type_to_nasm_type(object.type);

                        if (type == "byte" || type == "word")
                            emit_text("movzx eax, " + type + " [" + line[1] + "]");
                        else
                            emit_text("mov eax, " + type + " [" + line[1] + "]");

                        break;
                    }

                    emit_text("ret");
                }else {
                    error(lineno, line, "in function " + function.name + " return statement must be a number, string, or object");
                }
            }
        } else {
            std::vector<std::string> args;
            size_t paren = line[0].find('(');
            std::string name = line[0].substr(0, paren);

            std::string first = line[0].substr(paren + 1);
            if (!first.empty())
                args.push_back(first);

            for (size_t i = 1; i < line.size(); i++)
                args.push_back(line[i]);

            if (!args.empty() && args.back().back() == ')')
                args.back().pop_back();

            if (!args.empty() && args.back().back() == ')') args.back().pop_back();

            if (args.size() == 1 && args[0].empty())
                args.clear();

            const char *arg_regs[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};

            if (args.size() > 6) {
                error(lineno, line, "Too many arguments");
            }

            if (name == "__asm__") {
                if (args.size() < 1)
                    error(lineno, line, "Not enough arguments for builtin __asm__");

                emit_text(escape_string(args[0].substr(1, args[0].size() - 2)));
                lineno++;
                continue;
            } else if (name == "add") {
                if (args.size() != 2)
                    error(lineno, line, "Invalid arguments for builtin add");

                for (const auto &object : module.data) {
                    if (object.name == args[0]) {
                        emit_text("add " + type_to_nasm_type(object.type) + " [" + args[0] + "], " + args[1]);
                        break;
                    }
                }

                lineno++;
                continue;
            } else if (name == "sub") {
                if (args.size() != 2)
                    error(lineno, line, "Invalid arguments for builtin sub");

                for (const auto &object : module.data) {
                    if (object.name == args[0]) {
                        emit_text("sub " + type_to_nasm_type(object.type) + " [" + args[0] + "], " + args[1]);
                        break;
                    }
                }

                lineno++;
                continue;
            } else if (name == "mul") {
                if (args.size() != 2)
                    error(lineno, line, "Invalid arguments for builtin mul");

                for (const auto &object : module.data) {
                    if (object.name == args[0]) {
                        emit_text("imul " + type_to_nasm_type(object.type) + " [" + args[0] + "], " + args[1]);
                        break;
                    }
                }

                lineno++;
                continue;
            } else if (name == "div") {
                if (args.size() != 2)
                    error(lineno, line, "Invalid arguments for builtin div");

                for (const auto &object : module.data) {
                    if (object.name != args[0])
                        continue;

                    std::string type = type_to_nasm_type(object.type);

                    if (type == "byte") {
                        emit_text("mov al, [" + args[0] + "]");
                        emit_text("xor ah, ah");
                        emit_text("div " + args[1]);
                        emit_text("mov [" + args[0] + "], al");
                    } else if (type == "word") {
                        emit_text("mov ax, [" + args[0] + "]");
                        emit_text("xor dx, dx");
                        emit_text("div " + args[1]);
                        emit_text("mov [" + args[0] + "], ax");
                    } else if (type == "dword") {
                        emit_text("mov eax, [" + args[0] + "]");
                        emit_text("xor edx, edx");
                        emit_text("div " + args[1]);
                        emit_text("mov [" + args[0] + "], eax");
                    } else if (type == "qword") {
                        emit_text("mov rax, [" + args[0] + "]");
                        emit_text("xor rdx, rdx");
                        emit_text("div " + args[1]);
                        emit_text("mov [" + args[0] + "], rax");
                    }

                    break;
                }

                lineno++;
                continue;
            }

            for (size_t i = 0; i < args.size(); i++) {
                const std::string &arg = args[i];

                if (is_number(arg)) {
                    emit_text("mov " + std::string(arg_regs[i]) + ", " + arg);
                } else if (arg.size() >= 2 && arg.front() == '"' && arg.back() == '"') {
                    std::string strname = gen_name("string");
                    std::string str = arg.substr(1, arg.size() - 2);
                    emit_data(strname + ": db \"" + escape_string(str) + "\", 0");
                    emit_text("mov " + std::string(arg_regs[i]) + ", " + strname);
                } else if (is_object(module, arg)) {
                    for (const auto &object : module.data) {
                        if (object.name != arg)
                            continue;

                        std::string type = type_to_nasm_type(object.type);

                        if (type == "byte" || type == "word")
                            emit_text("movzx " + std::string(arg_regs[i]) + ", " + type + " [" + arg + "]");
                        else
                            emit_text("mov " + std::string(arg_regs[i]) + ", " + type + " [" + arg + "]");

                        break;
                    }
                } else {
                    error(lineno, line, "in function " + function.name + ": Undefined object");
                }
            }

            emit_text("xor eax, eax");
            emit_text("call " + name);
            emit_line("extern " + name);
        }

        lineno++;
    }
}

void compile_module(const Module &module) {
    for (const auto &object : module.data) {
        if (object.type == "db")
            emit_data(object.name + ": db " + object.value);
        else if (object.type == "dw")
            emit_data(object.name + ": dw " + object.value);
        else if (object.type == "dd")
            emit_data(object.name + ": dq " + object.value);
        else if (object.type == "dq")
            emit_data(object.name + ": dq " + object.value);
        else if (object.type == "string")
            emit_data(object.name + ": db " + object.value + ", 0");
    }

    for (const auto &function : module.functions)
        compile_function(function, module);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: gseac <source> <output>\n";
        return 1;
    }

    src = read_file(argv[1]);
    std::string temp = make_temp_dir();
    std::string asm_file = temp + "/output.asm";
    std::string obj_file = temp + "/output.o";

    out.open(asm_file);

    emit_line("bits 64");
    Module program = make_module(src);
    compile_module(program);

    out << "section .text\n";
    out << text.str();
    out << "\nsection .data\n";
    out << data.str();
    out.close();

    char *nargv[] = {(char *)"nasm", (char *)"-f", (char *)"elf64", (char *)asm_file.c_str(), (char *)"-o", (char *)obj_file.c_str(), nullptr};
    exec("/usr/bin/nasm", nargv);

    char *gcc_argv[] = {(char *)"gcc", (char *)obj_file.c_str(), (char *)"-o", argv[2], (char *)"-no-pie", nullptr};
    exec("/usr/bin/gcc", gcc_argv);
}
