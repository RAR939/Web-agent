#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <fstream>
#include <vector>
#include <sstream>
#include <memory>
#include <stdexcept>
#include <filesystem>
#include <ctime>
#include <iomanip>
#include <cstdlib>
#include <mutex>
#include <atomic>
#include <csignal>
#include <algorithm>

#ifdef _WIN32
    #include <windows.h>
    #include <direct.h>
    #define MKDIR(path) _mkdir(path)
#else
    #include <sys/stat.h>
    #include <sys/wait.h>
    #include <unistd.h>
    #define MKDIR(path) mkdir(path, 0755)
#endif

#include <curl/curl.h>
#include "json.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;


std::atomic<bool> running(true);

void signalHandler(int signal) {
    running = false;
    std::cout << "\nЗавершение работы..." << std::endl;
}



std::tm getLocalTime(const std::time_t& time) {
    std::tm tm;
#ifdef _WIN32
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif
    return tm;
}

int executeCommand(const std::string& cmd) {
#ifdef _WIN32
    return system(cmd.c_str());
#else
    int status = system(cmd.c_str());
    return WEXITSTATUS(status);
#endif
}

std::string normalizePath(std::string path) {
#ifdef _WIN32
    std::replace(path.begin(), path.end(), '/', '\\');
#else
    std::replace(path.begin(), path.end(), '\\', '/');
#endif
    return path;
}

std::string getExeExtension() {
#ifdef _WIN32
    return ".exe";
#else
    return "";
#endif
}

std::string safeFilename(const std::string& name) {
    std::string safe = name;
    const std::string invalid = "\\/:*?\"<>| ";
    for (auto& c : safe) {
        if (invalid.find(c) != std::string::npos) c = '_';
    }
    if (safe.empty()) safe = "no_param";
    if (safe.length() > 50) safe = safe.substr(0, 50);
    return safe;
}



class Logger {
private:
    std::string log_file;
    std::ofstream log_stream;
    std::mutex log_mutex;
    
public:
    Logger(const std::string& filename) : log_file(filename) {
        log_stream.open(log_file, std::ios::app);
    }
    
    ~Logger() { if (log_stream.is_open()) log_stream.close(); }
    
    void write(const std::string& level, const std::string& message) {
        std::lock_guard<std::mutex> lock(log_mutex);
        
        auto now = std::chrono::system_clock::now();
        auto tt = std::chrono::system_clock::to_time_t(now);
        auto tm = getLocalTime(tt);
        
        std::stringstream ts;
        ts << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
        std::string msg = ts.str() + " [" + level + "] " + message;
        
        if (log_stream.is_open()) log_stream << msg << std::endl;
        
#ifndef _WIN32
        if (level == "ERROR") std::cout << "\033[1;31m" << msg << "\033[0m" << std::endl;
        else if (level == "WARN") std::cout << "\033[1;33m" << msg << "\033[0m" << std::endl;
        else if (level == "OK") std::cout << "\033[1;32m" << msg << "\033[0m" << std::endl;
        else if (level == "TASK") std::cout << "\033[1;36m" << msg << "\033[0m" << std::endl;
        else std::cout << msg << std::endl;
#else
        std::cout << msg << std::endl;
#endif
    }
    
    void info(const std::string& m) { write("INFO", m); }
    void error(const std::string& m) { write("ERROR", m); }
    void warn(const std::string& m) { write("WARN", m); }
    void ok(const std::string& m) { write("OK", m); }
    void task(const std::string& m) { write("TASK", m); }
    void debug(const std::string& m) { write("DEBUG", m); }
};

std::unique_ptr<Logger> logger;


struct Config {
    std::string uid;
    std::string descr;
    std::string access_code;
    int poll_interval = 10;
    int max_interval = 300;
    int min_interval = 5;
    std::string api_url;
    std::string agent_path = "./agent";
    std::string result_dir = "./results";
    std::string log_file = "agent.log";
    bool debug = false;
    
    void normalize() {
        agent_path = normalizePath(agent_path);
        result_dir = normalizePath(result_dir);
        log_file = normalizePath(log_file);
        if (agent_path.find('.') == std::string::npos) {
            agent_path += getExeExtension();
        }
    }
};

struct TaskResult {
    int exit_code = -1;
    std::string message;
    std::string output;
    std::vector<std::string> files;
};



size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* output) {
    size_t total = size * nmemb;
    output->append((char*)contents, total);
    return total;
}

std::string httpPost(const std::string& url, const json& data, int timeout = 30) {
    CURL* curl = curl_easy_init();
    std::string response;
    
    if (curl) {
        std::string json_str = data.dump();
        
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_str.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, json_str.length());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        
        curl_easy_perform(curl);
        
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }
    
    return response;
}

bool checkServer(const std::string& url, int timeout = 5) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return res == CURLE_OK;
}



Config readConfig(const std::string& path) {
    Config cfg;
    std::ifstream f(path);
    if (!f.is_open()) throw std::runtime_error("Cannot open config: " + path);
    
    json j;
    f >> j;
    
    cfg.uid = j.value("uid", "");
    cfg.descr = j.value("descr", "");
    cfg.access_code = j.value("access_code", "");
    cfg.poll_interval = j.value("poll_interval_seconds", 10);
    cfg.max_interval = j.value("max_poll_interval_seconds", 300);
    cfg.min_interval = j.value("min_poll_interval_seconds", 5);
    cfg.api_url = j.value("api_base_url", "");
    cfg.agent_path = j.value("agent_path", "./agent");
    cfg.result_dir = j.value("result_directory", "./results");
    cfg.log_file = j.value("log_file", "agent.log");
    cfg.debug = j.value("debug_mode", false);
    
    if (cfg.uid.empty() || cfg.api_url.empty()) {
        throw std::runtime_error("Missing required fields");
    }
    
    cfg.normalize();
    
    std::error_code ec;
    fs::create_directories(cfg.result_dir, ec);
    
    return cfg;
}



class WebAgent {
private:
    Config cfg;
    std::string config_path;
    int interval;
    int fails;
    
    bool saveConfig() {
        json j = {
            {"uid", cfg.uid}, {"descr", cfg.descr},
            {"access_code", cfg.access_code},
            {"poll_interval_seconds", cfg.poll_interval},
            {"max_poll_interval_seconds", cfg.max_interval},
            {"min_poll_interval_seconds", cfg.min_interval},
            {"api_base_url", cfg.api_url},
            {"agent_path", cfg.agent_path},
            {"result_directory", cfg.result_dir},
            {"log_file", cfg.log_file},
            {"debug_mode", cfg.debug}
        };
        std::ofstream f(config_path);
        if (!f.is_open()) return false;
        f << j.dump(4);
        return true;
    }
    
    void adjustInterval(bool serverOk, bool hasTask) {
        if (!serverOk) {
            fails++;
            interval = std::min(cfg.poll_interval * (1 << std::min(fails, 5)), cfg.max_interval);
        } else {
            fails = 0;
            interval = hasTask ? cfg.min_interval : cfg.poll_interval;
        }
    }

    
    bool registerAgent() {
        logger->info("Регистрация агента...");
        
        json req = {{"UID", cfg.uid}, {"descr", cfg.descr}};
        if (!cfg.access_code.empty()) req["access_code"] = cfg.access_code;
        
        std::string resp = httpPost(cfg.api_url + "/wa_reg/", req);
        
        if (resp.empty()) { logger->error("Пустой ответ"); return false; }
        
        try {
            json j = json::parse(resp);
            std::string code = j.value("code_response", j.value("code_responce", ""));
            
            if (code == "0" || code == "-3") {
                if (j.contains("access_code")) {
                    cfg.access_code = j["access_code"].get<std::string>();
                    saveConfig();
                    logger->ok("Access code: " + cfg.access_code);
                }
                logger->ok("Агент зарегистрирован");
                return true;
            }
            logger->error("Код: " + code);
            return false;
        } catch (...) {
            logger->error("Ошибка парсинга");
            return false;
        }
    }
    

    
    TaskResult handleTASK(const json& task) {
        TaskResult res;
        res.exit_code = -1;
        
        std::string options = task.value("options", "");
        std::string session_id = task.value("session_id", "unknown");
        
        logger->task(" TASK: Запуск агента ");
        logger->info("Session: " + session_id);
        logger->info("Параметр: " + options);
        
        auto now = std::chrono::system_clock::now();
        auto tt = std::chrono::system_clock::to_time_t(now);
        auto tm = getLocalTime(tt);
        
        std::string safe = safeFilename(options);
        
        std::stringstream fn;
        fn << cfg.result_dir << "/task_" << "_"
           << std::put_time(&tm, "%Y%m%d_%H%M%S") << ".txt";
        
        std::string output_file = normalizePath(fn.str());
        
#ifdef _WIN32
        std::string cmd = cfg.agent_path + " --param " + options + " --output " + output_file;
#else
        std::string cmd = cfg.agent_path + " --param " + options + " --output " + output_file;
#endif
        
        logger->info("Команда: " + cmd);
        
        res.exit_code = executeCommand(cmd);
        
        std::ifstream f(output_file);
        if (f.is_open()) {
            std::stringstream buf;
            buf << f.rdbuf();
            res.output = buf.str();
            f.close();
            if (fs::exists(output_file)) {
                res.files.push_back(output_file);
            }
        } else {
            res.output = "exit_code=" + std::to_string(res.exit_code);
        }
        
        if (res.exit_code == 0) {
            res.message = "задание выполнено успешно";
            logger->ok("OK");
        } else {
            res.message = "задание не выполнено";
            logger->error("Код: " + std::to_string(res.exit_code));
        }
        
        return res;
    }
    
    TaskResult handleTIMEOUT(const json& task) {
        TaskResult res;
        std::string options = task.value("options", "");
        
        logger->task("═══ TIMEOUT ═══");
        logger->info("Интервал: " + options);
        
        try {
            int ni = std::stoi(options);
            if (ni < cfg.min_interval) ni = cfg.min_interval;
            if (ni > cfg.max_interval) ni = cfg.max_interval;
            
            int old = cfg.poll_interval;
            cfg.poll_interval = ni;
            interval = ni;
            
            if (saveConfig()) {
                res.exit_code = 0;
                res.message = "интервал изменён";
                res.output = std::to_string(old) + " -> " + std::to_string(ni);
                logger->ok("OK");
            } else {
                res.message = "ошибка сохранения";
                logger->error("Ошибка сохранения");
            }
        } catch (...) {
            res.message = "неверный формат";
        }
        return res;
    }
    
    TaskResult handleFILE(const json& task) {
        TaskResult res;
        res.exit_code = -1;
        
        std::string filename = task.value("options", "");
        std::string session_id = task.value("session_id", "unknown");
        
        logger->task("═══ FILE: Отправка файла на сервер ═══");
        logger->info("Session: " + session_id);
        logger->info("Файл: " + filename);
        
        if (filename.empty()) {
            res.message = "пустое имя файла";
            logger->error("Имя файла не указано");
            return res;
        }
        
        // Ищем файл в папке результатов
        std::string file_path = normalizePath(cfg.result_dir + "/" + filename);
        
        if (!fs::exists(file_path)) {
            // Может быть файл в текущей папке
            file_path = filename;
            if (!fs::exists(file_path)) {
                res.message = "файл не найден: " + filename;
                res.output = "File not found: " + filename;
                logger->error("Файл не найден: " + filename);
                return res;
            }
        }
        
        res.exit_code = 0;
        res.files.push_back(file_path);
        res.message = "файл загружен";
        res.output = "File uploaded: " + filename;
        
        auto size = fs::file_size(file_path);
        logger->ok("Файл найден: " + filename + " (" + std::to_string(size) + " байт)");
        
        return res;
    }
    
    TaskResult handleCONF(const json& task) {
        TaskResult res;
        res.exit_code = -1;
        
        std::string options = task.value("options", "");
        std::string session_id = task.value("session_id", "unknown");
        
        logger->task("═══ CONF ═══");
        logger->info("Options: " + options);
        
        if (options.empty()) {
            res.message = "нет параметров";
            return res;
        }
        
        // Разбираем параметр=значение
        size_t eq = options.find('=');
        if (eq == std::string::npos) {
            res.message = "неверный формат";
            return res;
        }
        
        std::string key = options.substr(0, eq);
        std::string val = options.substr(eq + 1);
        
        // Читаем текущий конфиг как JSON
        std::ifstream f(config_path);
        json j;
        f >> j;
        f.close();
        
        // Сохраняем старое значение для ответа
        std::string old_val = j.contains(key) ? j[key].dump() : "none";
        
        // Меняем значение (автоматически определится тип)
        if (val == "true" || val == "false") {
            j[key] = (val == "true");
        } else {
            try {
                j[key] = std::stoi(val);  // пробуем число
            } catch (...) {
                j[key] = val;  // иначе строка
            }
        }
        
        // Сохраняем обратно
        std::ofstream out(config_path);
        out << j.dump(4);
        out.close();
        
        // Обновляем текущий объект конфига
        cfg = readConfig(config_path);
        interval = cfg.poll_interval;
        
        res.exit_code = 0;
        res.message = "конфигурация обновлена";
        res.output = key + ": " + old_val + " -> " + val;
        logger->ok("OK: " + key + " = " + val);
        
        return res;
    }
    
    bool sendResult(const json& task, const TaskResult& res) {
        logger->info("Отправка результата...");
        
        std::string url = cfg.api_url + "/wa_result/";
        std::string session_id = task.value("session_id", "");
        int file_count = (int)res.files.size();
        
        // result_json строго по спецификации API
        json result_json = {
            {"UID", cfg.uid},
            {"access_code", cfg.access_code},
            {"message", res.message},
            {"files", file_count},
            {"session_id", session_id}
        };
        
        if (cfg.debug) {
            logger->debug("result_json: " + result_json.dump());
        }
        
        std::string response;
        
        // multipart/form-data
        CURL* curl = curl_easy_init();
        if (curl) {
            curl_mime* mime = curl_mime_init(curl);
            curl_mimepart* part;
            
            // result_code: 0 или код ошибки
            part = curl_mime_addpart(mime);
            curl_mime_name(part, "result_code");
            curl_mime_data(part, std::to_string(res.exit_code).c_str(), CURL_ZERO_TERMINATED);
            
            // result: JSON строка
            std::string json_str = result_json.dump();
            part = curl_mime_addpart(mime);
            curl_mime_name(part, "result");
            curl_mime_data(part, json_str.c_str(), CURL_ZERO_TERMINATED);
            
            // file1, file2, file3...
            for (size_t i = 0; i < res.files.size(); i++) {
                if (fs::exists(res.files[i])) {
                    part = curl_mime_addpart(mime);
                    std::string field = "file" + std::to_string(i + 1);
                    curl_mime_name(part, field.c_str());
                    curl_mime_filedata(part, res.files[i].c_str());
                    logger->debug("file" + std::to_string(i+1) + ": " + res.files[i]);
                }
            }
            
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
            
            curl_easy_perform(curl);
            
            curl_mime_free(mime);
            curl_easy_cleanup(curl);
        }
        
        if (cfg.debug) {
            logger->debug("Ответ: " + response);
        }
        
        try {
            json j = json::parse(response);
            std::string code = j.value("code_responce", j.value("code_response", ""));
            
            if (code == "0") {
                logger->ok("✓ Результат отправлен");
                return true;
            }
            
            logger->error("Код: " + code);
            if (j.contains("msg")) logger->error(j["msg"].get<std::string>());
            if (j.contains("status")) logger->error("Status: " + j["status"].get<std::string>());
            
        } catch (...) {
            logger->error("Ошибка парсинга ответа");
        }
        
        return false;
    }
    
    
    void pollServer() {
        json req = {
            {"UID", cfg.uid},
            {"descr", cfg.descr},
            {"access_code", cfg.access_code}
        };
        
        std::string resp = httpPost(cfg.api_url + "/wa_task/", req);
        
        if (resp.empty()) {
            adjustInterval(false, false);
            return;
        }
        
        try {
            json j = json::parse(resp);
            std::string code = j.value("code_response", j.value("code_responce", ""));
            
            if (code == "1") {
                TaskResult result = executeTask(j);
                sendResult(j, result);
                adjustInterval(true, true);
            } else if (code == "0") {
                if (cfg.debug) logger->debug("Нет заданий");
                adjustInterval(true, false);
            } else if (code == "-2") {
                logger->error("Неверный access_code");
            } else {
                logger->warn("Код: " + code);
            }
        } catch (...) {
            logger->error("Ошибка парсинга");
        }
    }
    
    TaskResult executeTask(const json& task) {
        std::string code = task.value("task_code", "");
        
        logger->task("   ЗАДАНИЕ: " + code + "               ");
        
        if (code == "TASK")    return handleTASK(task);
        if (code == "TIMEOUT") return handleTIMEOUT(task);
        if (code == "FILE")    return handleFILE(task);
        if (code == "CONF")    return handleCONF(task);
        
        TaskResult res;
        res.message = "неизвестный код: " + code;
        return res;
    }

public:
    WebAgent(const Config& c, const std::string& path) : cfg(c), config_path(path) {
        interval = cfg.poll_interval;
        fails = 0;
    }
    
    void run() {

        logger->info("  ВЕБ-АГЕНТ ");
        logger->info("UID: " + cfg.uid);
        logger->info("Сервер: " + cfg.api_url);

        logger->info("Агент: " + cfg.agent_path);
        logger->info("Интервал: " + std::to_string(cfg.poll_interval) + "с");
        
        int retry = 0;
        while (!registerAgent() && retry < 10 && running) {
            retry++;
            for (int i = 0; i < 30 && running; i++)
                std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
        if (!running) return;
        logger->ok("Готов к работе");
        
#ifdef _WIN32
        signal(SIGINT, signalHandler);
        signal(SIGTERM, signalHandler);
#else
        struct sigaction sa;
        sa.sa_handler = signalHandler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGINT, &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);
#endif
        
        while (running) {
            if (checkServer(cfg.api_url)) {
                pollServer();
            } else {
                adjustInterval(false, false);
            }
            
            for (int i = 0; i < interval && running; i++)
                std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
        logger->info("Работа завершена");
    }
};



int main(int argc, char* argv[]) {
    curl_global_init(CURL_GLOBAL_ALL);
    
    try {
        std::string config_path = "config.json";
        if (argc > 1) config_path = argv[1];
        
        if (!fs::exists(config_path)) {
            json def = {
                {"uid", "agent_001"},
                {"descr", "Web Agent v7.0"},
                {"access_code", ""},
                {"poll_interval_seconds", 10},
                {"max_poll_interval_seconds", 300},
                {"min_poll_interval_seconds", 5},
                {"api_base_url", "http://localhost:8080/api"},
                {"agent_path", "./agent"},
                {"result_directory", "./results"},
                {"log_file", "agent.log"},
                {"debug_mode", true}
            };
            std::ofstream f(config_path);
            f << def.dump(4);
            f.close();
            std::cout << "Создан config.json. Отредактируйте и запустите снова." << std::endl;
            return 1;
        }
        
        Config cfg = readConfig(config_path);
        logger = std::make_unique<Logger>(cfg.log_file);
        
        WebAgent agent(cfg, config_path);
        agent.run();
        
    } catch (const std::exception& e) {
        std::cerr << "Ошибка: " << e.what() << std::endl;
        curl_global_cleanup();
        return 1;
    }
    
    curl_global_cleanup();
    return 0;
}