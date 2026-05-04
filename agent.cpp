#include <iostream>
#include <fstream>
#include <string>
#include <chrono>
#include <ctime>
#include <cstdlib>


std::string getCurrentTime() {
    auto now = std::chrono::system_clock::now();
    std::time_t time = std::chrono::system_clock::to_time_t(now);
    char buffer[100];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", std::localtime(&time));
    return std::string(buffer);
}

void writeResultToFile(const std::string& filename, const std::string& param, int returnCode) {
    std::ofstream file(filename, std::ios::app);
    if (file.is_open()) {
        file << "Результат выполнения:\n";
        file << "Время: " << getCurrentTime() << "\n";
        file << "Параметр: " << param << "\n";
        file << "Код возврата: " << returnCode << "\n";
        
        if (returnCode == 0) {
            file << "Статус: успешно\n";
        } else {
            file << "Статус: ошибка\n";
        }
        
        file.close();
        std::cout << "Результат записан в файл: " << filename << std::endl;
    } else {
        std::cout << "Ошибка: не удалось открыть файл " << filename << std::endl;
    }
}

int main(int argc, char* argv[]) {
    std::string parametr = "unknown";
    std::string outputFile = "result.txt";
    
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--param" && i + 1 < argc) {
            parametr = argv[++i];
        } else if (arg == "--output" && i + 1 < argc) {
            outputFile = argv[++i];
        }
    }

    int returnCode = 0;
    
    writeResultToFile(outputFile, parametr, returnCode);
    
    std::cout << "Завершено с кодом: " << returnCode << std::endl;
    return returnCode;
}