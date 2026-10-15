#include "database.h"

#include <iostream>
#include <stdexcept>

#include <pqxx/pqxx>

namespace {

void EnsureSchema(pqxx::connection& conn) {
    pqxx::work tx{conn};
    tx.exec(R"(
CREATE TABLE IF NOT EXISTS retired_players (
    id SERIAL PRIMARY KEY,
    name TEXT NOT NULL,
    score NUMERIC NOT NULL,
    play_time REAL NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_retired_players_order
ON retired_players (score DESC, play_time ASC, name ASC);
)");
    tx.commit();
}

}  // namespace

Database::Database(std::string connection_string)
    : connection_string_{std::move(connection_string)} {
    pqxx::connection conn{connection_string_};
    EnsureSchema(conn);
    std::cout << "Database initialized" << std::endl;
}

void Database::SaveRetiredPlayer(const std::string& name, double score, double play_time) {
    pqxx::connection conn{connection_string_};
    pqxx::work tx{conn};
    tx.exec_params(
        "INSERT INTO retired_players (name, score, play_time) VALUES ($1, $2, $3)",
        name, score, play_time);
    tx.commit();
}

std::vector<RetiredPlayerRecord> Database::GetRecords(int start, int max_items) {
    pqxx::connection conn{connection_string_};
    pqxx::work tx{conn};
    auto result = tx.exec_params(
        R"(
SELECT name, score, play_time
FROM retired_players
ORDER BY score DESC, play_time ASC, name ASC
LIMIT $1 OFFSET $2
)",
        max_items, start);

    std::vector<RetiredPlayerRecord> records;
    records.reserve(result.size());
    for (const auto& row : result) {
        records.push_back({
            row[0].as<std::string>(),
            row[1].as<double>(),
            row[2].as<double>()
        });
    }
    tx.commit();
    return records;
}
