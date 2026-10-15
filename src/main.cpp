#include "database.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

struct Player {
    std::string id;
    std::string name;
    std::vector<double> pos{0.0, 0.0};
    std::vector<double> speed{0.0, 0.0};
    std::string dir = "U";
    struct BagItem {
        int id;
        int type;
    };
    std::vector<BagItem> bag;
    int score = 0;
    double join_time = 0.0;
    double idle_since = 0.0;
    bool is_idle = true;
};

struct LostObject {
    int type;
    std::vector<double> pos;
};

struct TokenData {
    std::string player_id;
    std::string map_id;
};

struct MapConfig {
    std::string id;
    std::string name;
    struct Road {
        double x0 = 0.0;
        double y0 = 0.0;
        double x1 = -1.0;
        double y1 = -1.0;
    };
    struct Office {
        double x = 0.0;
        double y = 0.0;
    };
    struct LootType {
        int value = 0;
    };
    std::vector<Road> roads;
    std::vector<Office> offices;
    std::vector<LootType> loot_types;
    std::map<int, int> loot_value_map;
};

class TokenStore {
public:
    void Add(const std::string& token, const TokenData& data) {
        std::lock_guard lock{mtx_};
        tokens_[token] = data;
    }

    bool Get(const std::string& token, TokenData& out) const {
        std::lock_guard lock{mtx_};
        if (auto it = tokens_.find(token); it != tokens_.end()) {
            out = it->second;
            return true;
        }
        return false;
    }

    void RemoveByPlayerId(const std::string& player_id) {
        std::lock_guard lock{mtx_};
        for (auto it = tokens_.begin(); it != tokens_.end();) {
            if (it->second.player_id == player_id) {
                it = tokens_.erase(it);
            } else {
                ++it;
            }
        }
    }

private:
    std::map<std::string, TokenData> tokens_;
    mutable std::mutex mtx_;
};

class Game {
public:
    void Reset() {
        players_.clear();
        lost_objects_.clear();
        map_id_.clear();
        next_player_id_ = 1;
        next_loot_object_id_ = 1;
        next_item_id_ = 1;
        map_timings_.clear();
        current_time_ = 0.0;
    }

    void SetRetirementTime(double seconds) {
        retirement_time_ = seconds;
    }

    double GetCurrentTime() const {
        return current_time_;
    }

    void GameLoop(const MapConfig& map, double delta_time, Database& db, TokenStore& tokens);

    std::string map_id_;
    std::map<std::string, Player> players_;
    std::map<int, LostObject> lost_objects_;
    int next_player_id_ = 1;
    int next_loot_object_id_ = 1;
    int next_item_id_ = 1;

    struct MapTiming {
        double accumulated_time = 0.0;
        double last_loot_generation_time = 0.0;
    };
    std::map<std::string, MapTiming> map_timings_;

    double loot_period_ms_ = 5000.0;
    double loot_probability_ = 0.5;

    mutable std::mutex mtx;

private:
    double current_time_ = 0.0;
    double retirement_time_ = 60.0;

    static double RandomDouble(double min, double max);
    static int RandomInt(int max);

    void GenerateLoot(const MapConfig& map, double current_time_ms);
    void MovePlayers(double delta_time);
    void CollectLoot();
    void DeliverItemsAtOffice(Player& player, const MapConfig& map);
    void CheckRetirement(Database& db, TokenStore& tokens);
    void SetPlayerIdle(Player& player);
    void SetPlayerMoving(Player& player);
};

class Server {
public:
    Server(std::string config_path, std::string www_root, Database& db);
    void Run(int port = 8080);

private:
    void SetupRoutes();
    void RespondJson(httplib::Response& res, const json& data, int status = 200) const;
    std::string GenerateToken() const;
    std::optional<std::string> ExtractMove(const json& body) const;
    void ApplyMove(Player& player, const std::string& move);

    std::string config_path_;
    std::string static_path_;
    std::map<std::string, MapConfig> maps_;
    Game game_;
    TokenStore tokens_;
    Database& db_;
    double retirement_time_ = 60.0;
    httplib::Server http_server_;
};

double Game::RandomDouble(double min, double max) {
    thread_local std::mt19937 gen{std::random_device{}()};
    std::uniform_real_distribution dist(min, max);
    return dist(gen);
}

int Game::RandomInt(int max) {
    thread_local std::mt19937 gen{std::random_device{}()};
    std::uniform_int_distribution dist(0, max - 1);
    return dist(gen);
}

void Game::SetPlayerIdle(Player& player) {
    if (!player.is_idle) {
        player.is_idle = true;
        player.idle_since = current_time_;
    }
}

void Game::SetPlayerMoving(Player& player) {
    player.is_idle = false;
}

void Game::GenerateLoot(const MapConfig& map, double current_time_ms) {
    auto& timing = map_timings_[map.id];
    if (current_time_ms - timing.last_loot_generation_time < loot_period_ms_) {
        return;
    }

    if (lost_objects_.size() >= players_.size()) {
        timing.last_loot_generation_time = current_time_ms;
        return;
    }

    if (map.roads.empty() || map.loot_types.empty()) {
        return;
    }

    const auto& road = map.roads[RandomInt(static_cast<int>(map.roads.size()))];
    double x = 0.0;
    double y = 0.0;
    if (road.x1 >= 0.0) {
        const double min_x = std::min(road.x0, road.x1);
        const double max_x = std::max(road.x0, road.x1);
        x = RandomDouble(min_x, max_x);
        y = road.y0;
    } else if (road.y1 >= 0.0) {
        const double min_y = std::min(road.y0, road.y1);
        const double max_y = std::max(road.y0, road.y1);
        y = RandomDouble(min_y, max_y);
        x = road.x0;
    } else {
        return;
    }

    x += 0.0001;
    y += 0.0001;
    const int type = RandomInt(static_cast<int>(map.loot_types.size()));
    const int id = next_loot_object_id_++;
    lost_objects_[id] = LostObject{type, {x, y}};
    timing.last_loot_generation_time = current_time_ms;
}

void Game::MovePlayers(double delta_time) {
    for (auto& [id, player] : players_) {
        const bool was_moving = player.speed[0] != 0.0 || player.speed[1] != 0.0;
        if (!was_moving) {
            continue;
        }
        player.pos[0] += player.speed[0] * delta_time;
        player.pos[1] += player.speed[1] * delta_time;
    }
}

void Game::CollectLoot() {
    std::vector<int> to_remove;
    for (const auto& [id, loot] : lost_objects_) {
        for (auto& [pid, player] : players_) {
            const double dx = std::abs(player.pos[0] - loot.pos[0]);
            const double dy = std::abs(player.pos[1] - loot.pos[1]);
            if (dx <= 0.5 && dy <= 0.5) {
                player.bag.push_back({next_item_id_++, loot.type});
                to_remove.push_back(id);
                break;
            }
        }
    }
    for (int id : to_remove) {
        lost_objects_.erase(id);
    }
}

void Game::DeliverItemsAtOffice(Player& player, const MapConfig& map) {
    bool on_office = false;
    for (const auto& office : map.offices) {
        if (std::abs(player.pos[0] - office.x) <= 0.5 &&
            std::abs(player.pos[1] - office.y) <= 0.5) {
            on_office = true;
            break;
        }
    }
    if (!on_office || player.bag.empty()) {
        return;
    }

    for (const auto& item : player.bag) {
        if (auto it = map.loot_value_map.find(item.type); it != map.loot_value_map.end()) {
            player.score += it->second;
        }
    }
    player.bag.clear();
}

void Game::CheckRetirement(Database& db, TokenStore& tokens) {
    std::vector<std::string> to_remove;
    for (const auto& [id, player] : players_) {
        if (player.is_idle && current_time_ - player.idle_since >= retirement_time_) {
            to_remove.push_back(id);
        }
    }

    for (const auto& id : to_remove) {
        auto it = players_.find(id);
        if (it == players_.end()) {
            continue;
        }
        const auto& player = it->second;
        const double play_time = current_time_ - player.join_time;
        try {
            db.SaveRetiredPlayer(player.name, static_cast<double>(player.score), play_time);
        } catch (const std::exception& ex) {
            std::cerr << "Failed to save retired player: " << ex.what() << std::endl;
        }
        tokens.RemoveByPlayerId(id);
        players_.erase(it);
    }
}

void Game::GameLoop(const MapConfig& map, double delta_time, Database& db, TokenStore& tokens) {
    if (map.id.empty()) {
        return;
    }

    current_time_ += delta_time;

    auto& timing = map_timings_[map.id];
    timing.accumulated_time += delta_time;
    const double current_time_ms = timing.accumulated_time * 1000.0;

    MovePlayers(delta_time);
    CollectLoot();
    for (auto& [id, player] : players_) {
        if (player.speed[0] == 0.0 && player.speed[1] == 0.0) {
            SetPlayerIdle(player);
        }
        DeliverItemsAtOffice(player, map);
    }
    GenerateLoot(map, current_time_ms);
    CheckRetirement(db, tokens);
}

Server::Server(std::string config_path, std::string www_root, Database& db)
    : config_path_{std::move(config_path)}
    , static_path_{std::move(www_root)}
    , db_{db} {
    std::ifstream ifs(config_path_);
    if (!ifs.is_open()) {
        std::cerr << "Failed to open config: " << config_path_ << std::endl;
        std::exit(1);
    }

    json config;
    ifs >> config;

    if (config.contains("lootGeneratorConfig")) {
        const auto& loot_cfg = config["lootGeneratorConfig"];
        if (loot_cfg.contains("period")) {
            game_.loot_period_ms_ = loot_cfg["period"].get<double>() * 1000.0;
        }
        if (loot_cfg.contains("probability")) {
            game_.loot_probability_ = loot_cfg["probability"].get<double>();
        }
    }

    if (config.contains("dogRetirementTime")) {
        retirement_time_ = config["dogRetirementTime"].get<double>();
    }
    game_.SetRetirementTime(retirement_time_);

    for (const auto& map_json : config["maps"]) {
        MapConfig map;
        map.id = map_json["id"].get<std::string>();
        map.name = map_json["name"].get<std::string>();

        for (const auto& road_json : map_json["roads"]) {
            MapConfig::Road road;
            road.x0 = road_json["x0"].get<double>();
            road.y0 = road_json["y0"].get<double>();
            if (road_json.contains("x1")) {
                road.x1 = road_json["x1"].get<double>();
            }
            if (road_json.contains("y1")) {
                road.y1 = road_json["y1"].get<double>();
            }
            map.roads.push_back(road);
        }

        for (const auto& office_json : map_json["offices"]) {
            map.offices.push_back({office_json["x"].get<double>(), office_json["y"].get<double>()});
        }

        for (size_t idx = 0; idx < map_json["lootTypes"].size(); ++idx) {
            const auto& loot_type = map_json["lootTypes"][idx];
            map.loot_types.push_back({loot_type["value"].get<int>()});
            map.loot_value_map[static_cast<int>(idx)] = loot_type["value"].get<int>();
        }

        maps_[map.id] = std::move(map);
    }

    SetupRoutes();
}

void Server::Run(int port) {
    std::cout << "Server started on port " << port << std::endl;
    http_server_.listen("0.0.0.0", port);
}

void Server::RespondJson(httplib::Response& res, const json& data, int status) const {
    res.set_header("Cache-Control", "no-cache");
    res.set_header("Content-Type", "application/json");
    res.status = status;
    res.body = data.dump();
    res.set_header("Content-Length", std::to_string(res.body.size()));
}

std::string Server::GenerateToken() const {
    thread_local std::mt19937 gen{std::random_device{}()};
    std::uniform_int_distribution dist(0, 15);
    static const char* hex = "0123456789abcdef";
    std::string token(32, '0');
    for (int i = 0; i < 32; ++i) {
        token[i] = hex[dist(gen)];
    }
    return token;
}

std::optional<std::string> Server::ExtractMove(const json& body) const {
    if (!body.contains("move")) {
        return std::string{};
    }
    const auto& move_value = body["move"];
    if (move_value.is_string()) {
        return move_value.get<std::string>();
    }
    if (move_value.is_array() && !move_value.empty()) {
        return move_value.back().get<std::string>();
    }
    return std::string{};
}

void Server::ApplyMove(Player& player, const std::string& move) {
    static const std::map<std::string, std::vector<double>> speed_map{
        {"U", {0.0, -1.0}},
        {"D", {0.0, 1.0}},
        {"L", {-1.0, 0.0}},
        {"R", {1.0, 0.0}},
    };

    if (!move.empty()) {
        if (auto it = speed_map.find(move); it != speed_map.end()) {
            player.speed = it->second;
            player.dir = move;
            player.is_idle = false;
            return;
        }
    }

    player.speed = {0.0, 0.0};
    player.is_idle = true;
    player.idle_since = game_.GetCurrentTime();
}

void Server::SetupRoutes() {
    http_server_.set_mount_point("/", static_path_);

    http_server_.Get("/api/v1/maps", [this](const httplib::Request&, httplib::Response& res) {
        json list = json::array();
        for (const auto& [id, map] : maps_) {
            list.push_back({{"id", map.id}, {"name", map.name}});
        }
        RespondJson(res, list);
    });

    http_server_.Get(R"(/api/v1/maps/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string id = req.matches[1];
        const auto it = maps_.find(id);
        if (it == maps_.end()) {
            RespondJson(res, {{"code", "mapNotFound"}, {"message", "Map not found"}}, 404);
            return;
        }

        const auto& map = it->second;
        json map_json;
        map_json["id"] = map.id;
        map_json["name"] = map.name;
        map_json["roads"] = json::array();
        for (const auto& road : map.roads) {
            json road_json;
            road_json["x0"] = road.x0;
            road_json["y0"] = road.y0;
            if (road.x1 >= 0.0) {
                road_json["x1"] = road.x1;
            }
            if (road.y1 >= 0.0) {
                road_json["y1"] = road.y1;
            }
            map_json["roads"].push_back(std::move(road_json));
        }
        map_json["offices"] = json::array();
        for (const auto& office : map.offices) {
            map_json["offices"].push_back({{"x", office.x}, {"y", office.y}});
        }
        RespondJson(res, map_json);
    });

    http_server_.Post("/api/v1/game/join", [this](const httplib::Request& req, httplib::Response& res) {
        const json body = json::parse(req.body);
        const std::string user_name = body.value("userName", "");
        const std::string map_id = body.value("mapId", "");

        if (user_name.empty()) {
            RespondJson(res, {{"code", "invalidArgument"}, {"message", "Invalid name"}}, 400);
            return;
        }
        if (!maps_.contains(map_id)) {
            RespondJson(res, {{"code", "mapNotFound"}, {"message", "Invalid map"}}, 400);
            return;
        }

        std::lock_guard lock{game_.mtx};

        if (game_.map_id_ != map_id || game_.players_.empty()) {
            game_.Reset();
            game_.map_id_ = map_id;
            game_.SetRetirementTime(retirement_time_);
        }

        const MapConfig& map = maps_.at(map_id);
        const std::vector<double> start_pos{
            map.offices[0].x + 0.0001,
            map.offices[0].y + 0.0001
        };

        const std::string player_id = std::to_string(game_.next_player_id_++);
        Player player;
        player.id = player_id;
        player.name = user_name;
        player.pos = start_pos;
        player.join_time = game_.GetCurrentTime();
        player.idle_since = game_.GetCurrentTime();
        player.is_idle = true;
        game_.players_[player_id] = std::move(player);

        const std::string token = GenerateToken();
        tokens_.Add(token, TokenData{player_id, map_id});

        RespondJson(res, {
            {"authToken", token},
            {"playerId", std::stoi(player_id)}
        });
    });

    http_server_.Post("/api/v1/game/player/action", [this](const httplib::Request& req, httplib::Response& res) {
        const auto auth = req.get_header_value("Authorization");
        if (auth.empty() || auth.rfind("Bearer ", 0) != 0) {
            RespondJson(res, {{"code", "invalidToken"}, {"message", "Authorization header is required"}}, 401);
            return;
        }

        const std::string token = auth.substr(7);
        TokenData token_data;
        if (!tokens_.Get(token, token_data)) {
            RespondJson(res, {{"code", "unknownToken"}, {"message", "Player token not found"}}, 401);
            return;
        }

        std::lock_guard lock{game_.mtx};
        const auto it = game_.players_.find(token_data.player_id);
        if (it == game_.players_.end()) {
            RespondJson(res, {{"code", "playerRetired"}, {"message", "Player has retired"}}, 401);
            return;
        }

        const json body = json::parse(req.body);
        ApplyMove(it->second, ExtractMove(body).value_or(""));

        RespondJson(res, {{"ok", true}});
    });

    http_server_.Get("/api/v1/game/state", [this](const httplib::Request& req, httplib::Response& res) {
        const auto auth = req.get_header_value("Authorization");
        if (auth.empty() || auth.rfind("Bearer ", 0) != 0) {
            RespondJson(res, {{"code", "invalidToken"}, {"message", "Authorization header is required"}}, 401);
            return;
        }

        const std::string token = auth.substr(7);
        TokenData token_data;
        if (!tokens_.Get(token, token_data)) {
            RespondJson(res, {{"code", "unknownToken"}, {"message", "Player token not found"}}, 401);
            return;
        }

        std::lock_guard lock{game_.mtx};
        if (!game_.players_.contains(token_data.player_id)) {
            RespondJson(res, {{"code", "playerRetired"}, {"message", "Player has retired"}}, 401);
            return;
        }

        json state;
        state["players"] = json::object();
        for (const auto& [id, player] : game_.players_) {
            json bag = json::array();
            for (const auto& item : player.bag) {
                bag.push_back({{"id", item.id}, {"type", item.type}});
            }
            state["players"][id] = {
                {"pos", player.pos},
                {"speed", player.speed},
                {"dir", player.dir},
                {"bag", std::move(bag)},
                {"score", player.score}
            };
        }

        state["lostObjects"] = json::object();
        for (const auto& [id, obj] : game_.lost_objects_) {
            state["lostObjects"][std::to_string(id)] = {
                {"type", obj.type},
                {"pos", obj.pos}
            };
        }

        std::string body = state.dump();
        const std::string old_str = "[0,0]";
        const std::string new_str = "[0.0,0.0]";
        for (size_t pos = 0; (pos = body.find(old_str, pos)) != std::string::npos;) {
            body.replace(pos, old_str.length(), new_str);
            pos += new_str.length();
        }

        res.set_header("Cache-Control", "no-cache");
        res.set_header("Content-Type", "application/json");
        res.status = 200;
        res.body = std::move(body);
        res.set_header("Content-Length", std::to_string(res.body.size()));
    });

    http_server_.Get("/api/v1/game/players", [this](const httplib::Request&, httplib::Response& res) {
        std::lock_guard lock{game_.mtx};
        json players_json = json::object();
        for (const auto& [id, player] : game_.players_) {
            players_json[id] = {{"name", player.name}, {"pos", player.pos}};
        }
        RespondJson(res, players_json);
    });

    http_server_.Post("/api/v1/game/tick", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            const json body = json::parse(req.body);
            if (!body.contains("timeDelta") || !body["timeDelta"].is_number()) {
                RespondJson(res, {{"error", "Invalid timeDelta"}}, 400);
                return;
            }
            const double time_delta = body["timeDelta"].get<double>();
            if (time_delta <= 0.0) {
                RespondJson(res, {{"error", "Invalid timeDelta"}}, 400);
                return;
            }

            std::lock_guard lock{game_.mtx};
            if (game_.map_id_.empty()) {
                RespondJson(res, {{"ok", true}});
                return;
            }

            const auto it = maps_.find(game_.map_id_);
            if (it != maps_.end()) {
                game_.GameLoop(it->second, time_delta / 1000.0, db_, tokens_);
            }
            RespondJson(res, {{"ok", true}});
        } catch (const std::exception&) {
            RespondJson(res, {{"error", "Internal server error"}}, 500);
        }
    });

    http_server_.Get("/api/v1/game/records", [this](const httplib::Request& req, httplib::Response& res) {
        int start = 0;
        int max_items = 100;

        if (req.has_param("start")) {
            start = std::stoi(req.get_param_value("start"));
            if (start < 0) {
                start = 0;
            }
        }
        if (req.has_param("maxItems")) {
            max_items = std::stoi(req.get_param_value("maxItems"));
        }

        if (max_items > 100) {
            RespondJson(res, {{"code", "invalidParam"}, {"message", "maxItems exceeds 100"}}, 400);
            return;
        }

        try {
            const auto records = db_.GetRecords(start, max_items);
            json result = json::array();
            for (const auto& record : records) {
                result.push_back({
                    {"name", record.name},
                    {"score", record.score},
                    {"playTime", record.play_time}
                });
            }
            RespondJson(res, result);
        } catch (const std::exception& ex) {
            std::cerr << "DB error: " << ex.what() << std::endl;
            RespondJson(res, {{"error", "Internal server error"}}, 500);
        }
    });

    const auto method_not_allowed = [this](const httplib::Request&, httplib::Response& res) {
        res.set_header("Allow", "GET, HEAD");
        RespondJson(res, {{"code", "invalidMethod"}, {"message", "Method not allowed"}}, 405);
    };

    http_server_.Post(R"(/api/v1/maps/(.+))", method_not_allowed);
    http_server_.Put(R"(/api/v1/maps/(.+))", method_not_allowed);
    http_server_.Delete(R"(/api/v1/maps/(.+))", method_not_allowed);
    http_server_.Post("/api/v1/game/state", method_not_allowed);
    http_server_.Put("/api/v1/game/state", method_not_allowed);
    http_server_.Delete("/api/v1/game/state", method_not_allowed);

    http_server_.set_error_handler([](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-cache");
        res.set_header("Content-Type", "application/json");
        res.status = 404;
        res.body = R"({"code":"notFound","message":"Not found"})";
        res.set_header("Content-Length", std::to_string(res.body.size()));
    });
}

int main(int argc, char* argv[]) {
    const char* db_url = std::getenv("GAME_DB_URL");
    if (db_url == nullptr || std::string{db_url}.empty()) {
        std::cerr << "GAME_DB_URL environment variable is not set" << std::endl;
        return EXIT_FAILURE;
    }

    std::string config_path = "./data/config.json";
    std::string static_path = "./static";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--config-file" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--www-root" && i + 1 < argc) {
            static_path = argv[++i];
        }
    }

    try {
        Database db{db_url};
        Server server{config_path, static_path, db};
        server.Run(8080);
    } catch (const std::exception& ex) {
        std::cerr << "Exception: " << ex.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
