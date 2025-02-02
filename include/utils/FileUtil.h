//
// Created by Kai Zhao on 12/9/19.
//

#ifndef _SZ_FILE_UTIL
#define _SZ_FILE_UTIL

#include <iostream>
#include <fstream>
#include <iomanip>
#include <cassert>
#include <memory>

namespace SZ {
    bool file_exist(const char *file) {
        std::ifstream infile(file);
        return infile.good();
    }


    template<typename Type>
    void readfile(const char *file, const size_t num, Type *data) {
        std::ifstream fin(file, std::ios::binary);
        if (!fin) {
            std::cout << " Error, Couldn't find the file" << "\n";
            return;
        }
        fin.seekg(0, std::ios::end);
        const size_t num_elements = fin.tellg() / sizeof(Type);
        assert(num_elements == num && "File size is not equals to the input setting");
        fin.seekg(0, std::ios::beg);
        fin.read(reinterpret_cast<char *>(data), num_elements * sizeof(Type));
        fin.close();
    }

    template<typename Type>
    std::unique_ptr<Type[]> readfile(const char *file, size_t &num) {
        std::ifstream fin(file, std::ios::binary);
        if (!fin) {
            std::cout << " Error, Couldn't find the file" << "\n";
            return 0;
        }
        fin.seekg(0, std::ios::end);
        const size_t num_elements = fin.tellg() / sizeof(Type);
        fin.seekg(0, std::ios::beg);
//        auto data = SZ::compat::make_unique<Type[]>(num_elements);
        auto data = std::make_unique<Type[]>(num_elements);
        fin.read(reinterpret_cast<char *>(&data[0]), num_elements * sizeof(Type));
        fin.close();
        num = num_elements;
        return data;
    }

    template<typename Type>
    std::unique_ptr<Type[]> readfile(const char *file, size_t start, size_t num) {
        std::ifstream fin(file, std::ios::binary);
        if (!fin) {
            std::cout << " Error, Couldn't find the file" << "\n";
            return 0;
        }
        fin.seekg(0, std::ios::end);
        const size_t total_num_elements = fin.tellg() / sizeof(Type);
        assert(start + num <= total_num_elements);
        fin.seekg(start * sizeof(Type), std::ios::beg);
        auto data = std::make_unique<Type[]>(num);
        fin.read(reinterpret_cast<char *>(&data[0]), num * sizeof(Type));
        fin.close();
        return data;
    }


    template<typename Type>
    void writefile(const char *file, Type *data, size_t num_elements) {
        std::ofstream fout(file, std::ios::binary);
        fout.write(reinterpret_cast<const char *>(&data[0]), num_elements * sizeof(Type));
        fout.close();
    }
}

#endif /*_SZ_CONCEPTS*/
