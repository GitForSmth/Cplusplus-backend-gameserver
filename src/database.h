#pragma once

#include <string>
#include <vector>
#include <optional>

struct RetiredPlayerRecord {
    std::string name;
    double score;
    double play_time;
};

class Database {
public:
    explicit Database(std::string connection_string);

    void SaveRetiredPlayer(const std::string& name, double score, double play_time);
    std::vector<RetiredPlayerRecord> GetRecords(int start, int max_items);

private:
    std::string connection_string_;
};
