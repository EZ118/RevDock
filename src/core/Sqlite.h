#pragma once

#include <functional>
#include <string>
#include <vector>

namespace revdock {

// Minimal read-only SQLite helper. Firefox and Visual Studio Code keep their
// recent history in SQLite databases; loading the library dynamically keeps
// RevDock free of a hard SQLite dependency on both Windows (winsqlite3.dll)
// and Linux (libsqlite3.so).
class Sqlite {
public:
    using Row = std::vector<std::string>;

    Sqlite();
    ~Sqlite();

    Sqlite(const Sqlite &) = delete;
    Sqlite &operator=(const Sqlite &) = delete;

    bool open(const std::string &path, bool readOnly = true);
    void close();
    bool valid() const { return db_ != nullptr; }

    // Calls `row` for every result row. Returning false stops the query.
    // `limit` is a safety cap in addition to any LIMIT in the SQL itself.
    bool query(const std::string &sql, const std::function<bool(const Row &)> &row,
               int limit = 0);

private:
    void *db_ = nullptr;
};

} // namespace revdock
