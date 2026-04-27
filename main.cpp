#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <fstream>
#include <vector>
#include <sstream>
#include <array>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;


struct Config {
    std::string uid;
    std::string descr;
    std::string access_code;
    int poll_interval_seconds;
    std::string api_base_url;
    std::string agent_path;       
    std::string result_directory;   
};


struct AgentResult {
    int exit_code;
    std::string stdout_output;
    std::string stderr_output;
    std::vector<std::string> result_files;  
};


Config readConfig(const std::string& filename);
size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* output);
std::string sendPostRequest(const std::string& url, const json& request_data);
std::string sendMultipartRequest(const std::string& url, const json& result_data, 
                                 const std::vector<std::string>& file_paths);
bool registerAgent(Config& config);
AgentResult executeAgent(const std::string& agent_path, const json& task);
void sendResult(const Config& config, const json& task, const AgentResult& agent_result);
void requestTask(const Config& config);
void saveAccessCode(const std::string& filename, const std::string& access_code);
std::vector<std::string> findResultFiles(const std::string& directory, const std::string& session_id);
std::string readFileContent(const std::string& filepath);


void saveAccessCode(const std::string& filename, const std::string& access_code) {
    try {
        std::ifstream in_file(filename);
        if (in_file.good()) {
            json config_json;
            in_file >> config_json;
            in_file.close();
            
            config_json["access_code"] = access_code;
            
            std::ofstream out_file(filename);
            out_file << config_json.dump(4);
            out_file.close();
            
            std::cout << "✓ Access code сохранен в config.json" << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "Ошибка сохранения access_code: " << e.what() << std::endl;
    }
}


std::string readFileContent(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}


std::vector<std::string> findResultFiles(const std::string& directory, const std::string& session_id) {
    std::vector<std::string> files;
    std::string command = "ls " + directory + "/*.result 2>/dev/null";
    std::array<char, 128> buffer;
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(command.c_str(), "r"), pclose);
    if (!pipe) {
        return files;
    }
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        std::string line(buffer.data());
        line.erase(line.find_last_not_of("\n") + 1);
        if (!line.empty()) {
            files.push_back(line);
        }
    }
    
    return files;
}


Config readConfig(const std::string& filename) {
    Config config;
    std::ifstream file(filename);
    if (!file.is_open()) {
        throw std::runtime_error("Не удалось открыть файл конфигурации: " + filename);
    }
    
    json config_json;
    file >> config_json;
    file.close();
    
    config.uid = config_json.value("uid", "");
    config.descr = config_json.value("descr", "");
    config.access_code = config_json.value("access_code", "");
    config.poll_interval_seconds = config_json.value("poll_interval_seconds", 10);
    config.api_base_url = config_json.value("api_base_url", "");
    config.agent_path = config_json.value("agent_path", "./agent");
    config.result_directory = config_json.value("result_directory", ".");
    
    if (config.uid.empty() || config.descr.empty() || config.api_base_url.empty()) {
        throw std::runtime_error("Не все обязательные поля заполнены в конфигурации");
    }
    
    return config;
}


size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* output) {
    size_t totalSize = size * nmemb;
    output->append((char*)contents, totalSize);
    return totalSize;
}


std::string sendPostRequest(const std::string& url, const json& request_data) {
    CURL* curl = curl_easy_init();
    std::string response;
    
    if (curl) {
        std::string json_str = request_data.dump();
        
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_str.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, json_str.length());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        
        CURLcode res = curl_easy_perform(curl);
        
        if (res != CURLE_OK) {
            response = "{\"error\": \"" + std::string(curl_easy_strerror(res)) + "\"}";
        }
        
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }
    
    return response;
}


std::string sendMultipartRequest(const std::string& url, const json& result_data, 
                                 const std::vector<std::string>& file_paths) {
    CURL* curl = curl_easy_init();
    std::string response;
    
    if (curl) {
        curl_mime* mime = curl_mime_init(curl);
        curl_mimepart* part;
        
        
        part = curl_mime_addpart(mime);
        curl_mime_name(part, "result_code");
        curl_mime_data(part, "0", CURL_ZERO_TERMINATED);
        
        std::string result_str = result_data.dump();
        part = curl_mime_addpart(mime);
        curl_mime_name(part, "result");
        curl_mime_data(part, result_str.c_str(), CURL_ZERO_TERMINATED);
        
        
        for (size_t i = 0; i < file_paths.size(); ++i) {
            std::string field_name = "file" + std::to_string(i + 1);
            part = curl_mime_addpart(mime);
            curl_mime_name(part, field_name.c_str());
            curl_mime_filedata(part, file_paths[i].c_str());
            std::cout << "Добавлен файл: " << file_paths[i] << " как " << field_name << std::endl;
        }
        
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        
        CURLcode res = curl_easy_perform(curl);
        
        if (res != CURLE_OK) {
            response = "{\"error\": \"" + std::string(curl_easy_strerror(res)) + "\"}";
        }
        
        curl_mime_free(mime);
        curl_easy_cleanup(curl);
    }
    
    return response;
}


bool registerAgent(Config& config) {
    std::string url = config.api_base_url + "/wa_reg/";
    json request = {
        {"UID", config.uid},
        {"descr", config.descr}
    };
    
    std::cout << "\n[Регистрация агента]" << std::endl;
    std::cout << "URL: " << url << std::endl;
    std::cout << "Запрос: " << request.dump() << std::endl;
    
    std::string response = sendPostRequest(url, request);
    std::cout << "Ответ: " << response << std::endl;
    
    if (response.empty()) {
        std::cerr << "Ошибка: пустой ответ от сервера" << std::endl;
        return false;
    }
    
    try {
        json resp_json = json::parse(response);
        
        std::string code;
        if (resp_json.contains("code_response")) {
            code = resp_json["code_response"];
        } else if (resp_json.contains("code_responce")) {
            code = resp_json["code_responce"];
        } else {
            std::cerr << "Ошибка: ответ не содержит code_response или code_responce" << std::endl;
            return false;
        }
        
        if (code == "0") {
            std::cout << "✓ Регистрация успешна!" << std::endl;
            if (resp_json.contains("access_code")) {
                std::string new_access_code = resp_json["access_code"];
                std::cout << "Получен access_code: " << new_access_code << std::endl;
                config.access_code = new_access_code;
                saveAccessCode("config.json", new_access_code);
            }
            return true;
        } else if (code == "-3") {
            std::cout << "✓ Агент уже зарегистрирован" << std::endl;
            return true;
        } else {
            std::cout << "✗ Ошибка регистрации. Код: " << code << std::endl;
            if (resp_json.contains("msg")) {
                std::cout << "Сообщение: " << resp_json["msg"] << std::endl;
            }
            return false;
        }
    } catch (const json::parse_error& e) {
        std::cerr << "Ошибка парсинга JSON: " << e.what() << std::endl;
        std::cerr << "Полученный ответ: '" << response << "'" << std::endl;
        return false;
    } catch (const std::exception& e) {
        std::cerr << "Ошибка: " << e.what() << std::endl;
        return false;
    }
}


AgentResult executeAgent(const std::string& agent_path, const json& task) {
    AgentResult result;
    result.exit_code = -1;

    time_t now = time(nullptr);
    struct tm* timeinfo = localtime(&now);
    

    char time_buffer[20];
    strftime(time_buffer, sizeof(time_buffer), "%Y%m%d_%H%M%S", timeinfo);
    
    
    std::string filename = "result_" + std::string(time_buffer) + ".txt";
    
    std::cout << "\n[Запуск внешнего агента]" << std::endl;
    std::cout << "Путь к агенту: " << agent_path << std::endl;
    
    
    std::string command = agent_path;
    
    
    if (task.contains("options") && !task["options"].get<std::string>().empty()) {
        command += " --param " + task["options"].get<std::string>() + " --output " + filename;  // Просто передаем значение как аргумент
        std::cout << "Параметр для агента: " << task["options"].get<std::string>() << std::endl;
    } else {
        std::cout << "Параметры для агента отсутствуют" << std::endl;
    }
    
    std::cout << "Команда: " << command << std::endl;
    
    
    std::array<char, 128> buffer;
    std::string stdout_result;
    
    
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(command.c_str(), "r"), pclose);
    if (!pipe) {
        std::cerr << "Ошибка: не удалось запустить агента" << std::endl;
        result.stderr_output = "Failed to execute agent";
        return result;
    }
    
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        stdout_result += buffer.data();
    }
    
    
    result.exit_code = pclose(pipe.release());
    
    
    if (result.exit_code != -1) {
        result.exit_code = WEXITSTATUS(result.exit_code);
    }
    
    result.stdout_output = stdout_result;
    
    std::cout << "Код возврата: " << result.exit_code << std::endl;
    std::cout << "Вывод агента:" << std::endl;
    std::cout << "--- START ---" << std::endl;
    std::cout << stdout_result;
    std::cout << "--- END ---" << std::endl;
    
    if (result.exit_code == 0) {
        std::cout << " Агент выполнен успешно" << std::endl;
        
         if (result.exit_code == 0) {
        std::cout << " Агент выполнен успешно" << std::endl;
        
        
        std::ifstream file(filename);
        if (file.good()) {
            result.result_files.push_back(filename);
            std::cout << "Найден файл результата: " << filename << std::endl;
        } else {
            std::cout << " Файл результата не найден: " << filename << std::endl;
        }
    } else {
        std::cout << " Агент завершился с ошибкой" << std::endl;
    }
    
    return result;
}
    
    return result;
}


void sendResult(const Config& config, const json& task, const AgentResult& agent_result) {
    std::string url = config.api_base_url + "/wa_result/";
    
    
    std::string message;
    if (agent_result.exit_code == 0) {
        message = "задание выполнено успешно. Вывод: " + agent_result.stdout_output;
    } else {
        message = "задание не выполнено. Код ошибки: " + std::to_string(agent_result.exit_code) + 
                  ". Вывод: " + agent_result.stdout_output;
    }
    
    json result_data = {
        {"UID", config.uid},
        {"access_code", config.access_code},
        {"message", message},
        {"files", agent_result.result_files.size()},
        {"session_id", task.value("session_id", "")},
        {"exit_code", agent_result.exit_code},
        {"stdout", agent_result.stdout_output}
    };
    
    std::cout << "\n[Отправка результата]" << std::endl;
    std::cout << "URL: " << url << std::endl;
    std::cout << "Количество файлов для отправки: " << agent_result.result_files.size() << std::endl;
    
    std::string response = sendMultipartRequest(url, result_data, agent_result.result_files);
    std::cout << "Ответ: " << response << std::endl;
    
    if (response.empty()) {
        std::cerr << "Ошибка: пустой ответ от сервера при отправке результата" << std::endl;
        return;
    }
    
    try {
        json resp_json = json::parse(response);
        std::string code;
        if (resp_json.contains("code_response")) {
            code = resp_json["code_response"];
        } else if (resp_json.contains("code_responce")) {
            code = resp_json["code_responce"];
        } else {
            std::cerr << "Ошибка: ответ не содержит code_response или code_responce" << std::endl;
            return;
        }
        
        if (code == "0") {
            std::cout << " Результат успешно отправлен!" << std::endl;
        } else {
            std::cout << " Ошибка при отправке результата: " << resp_json.value("msg", "") << std::endl;
            if (resp_json.contains("status")) {
                std::cout << "Статус: " << resp_json["status"] << std::endl;
            }
        }
    } catch (const json::parse_error& e) {
        std::cerr << "Ошибка парсинга JSON ответа: " << e.what() << std::endl;
        std::cerr << "Ответ сервера: '" << response << "'" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Ошибка: " << e.what() << std::endl;
    }
}


void requestTask(const Config& config) {
    std::string url = config.api_base_url + "/wa_task/";
    json request = {
        {"UID", config.uid},
        {"descr", config.descr},
        {"access_code", config.access_code}
    };
    
    std::cout << "\n[Запрос задания]" << std::endl;
    std::cout << "URL: " << url << std::endl;
    
    std::string response = sendPostRequest(url, request);
    std::cout << "Ответ: " << response << std::endl;
    
    if (response.empty()) {
        std::cerr << "Ошибка: пустой ответ от сервера" << std::endl;
        return;
    }
    
    try {
        json resp_json = json::parse(response);
        
        std::string code;
        if (resp_json.contains("code_response")) {
            code = resp_json["code_response"];
        } else if (resp_json.contains("code_responce")) {
            code = resp_json["code_responce"];
        } else {
            std::cerr << "Ошибка: ответ не содержит code_response или code_responce" << std::endl;
            return;
        }
        
        if (code == "1") {
            std::cout << "Получено задание!" << std::endl;
            std::cout << "Детали задания:" << std::endl;
            std::cout << "  Task code: " << resp_json.value("task_code", "UNKNOWN") << std::endl;
            std::cout << "  Options: " << resp_json.value("options", "") << std::endl;
            std::cout << "  Session ID: " << resp_json.value("session_id", "") << std::endl;
            std::cout << "  Status: " << resp_json.value("status", "UNKNOWN") << std::endl;
            
            // Запускаем внешний агент
            AgentResult agent_result = executeAgent(config.agent_path, resp_json);
            
            // Отправляем результат на сервер
            sendResult(config, resp_json, agent_result);
            
        } else if (code == "0") {
            std::cout << "○ Заданий нет. Статус: " << resp_json.value("status", "WAIT") << std::endl;
        } else if (code == "-2") {
            std::cout << " Ошибка: неверный код доступа" << std::endl;
        } else {
            std::cout << "Другой код ответа: " << code << std::endl;
            if (resp_json.contains("msg")) {
                std::cout << "Сообщение: " << resp_json["msg"] << std::endl;
            }
        }
    } catch (const json::parse_error& e) {
        std::cerr << "Ошибка парсинга JSON: " << e.what() << std::endl;
        std::cerr << "Полученный ответ: '" << response << "'" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Ошибка: " << e.what() << std::endl;
    }
}

int main() {

    curl_global_init(CURL_GLOBAL_ALL);
    
    try {

        Config config = readConfig("config.json");
        
        std::cout << "UID: " << config.uid << std::endl;
        std::cout << "Description: " << config.descr << std::endl;
        std::cout << "Access Code: " << (config.access_code.empty() ? "не установлен" : config.access_code) << std::endl;
        std::cout << "Poll interval: " << config.poll_interval_seconds << " seconds" << std::endl;
        std::cout << "API URL: " << config.api_base_url << std::endl;
        std::cout << "Agent path: " << config.agent_path << std::endl;
        std::cout << "Result directory: " << config.result_directory << std::endl;
        

        if (!registerAgent(config)) {
            std::cerr << "\nНе удалось зарегистрировать агента!" << std::endl;
            std::cerr << "Проверьте подключение к серверу и правильность URL" << std::endl;
            curl_global_cleanup();
            return 1;
        }
        
        std::cout << "\nНачинаем опрос сервера..." << std::endl;
        
        
        int iteration = 1;
        while (true) {
            std::cout << "Итерация " << iteration++ << std::endl;

            
            requestTask(config);
            
            std::cout << "\nОжидание " << config.poll_interval_seconds << " секунд..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(config.poll_interval_seconds));
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Критическая ошибка: " << e.what() << std::endl;
        curl_global_cleanup();
        return 1;
    }
    
    curl_global_cleanup();
    return 0;
}