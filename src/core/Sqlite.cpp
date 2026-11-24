#include "core/Sqlite.h"

#include "core/Log.h"

#include <algorithm>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace revdock {
namespace {

struct SqliteApi {
    using OpenV2 = int (*)(const char *, void **, int, const char *);
    using Close = int (*)(void *);
    using PrepareV2 = int (*)(void *, const char *, int, void **, const char **);
    using Step = int (*)(void *);
    using Finalize = int (*)(void *);
    using ColumnText = const unsigned char *(*)(void *, int);
    using ColumnType = int (*)(void *, int);
    using ColumnCount = int (*)(void *);

    void *module = nullptr;
    OpenV2 openV2 = nullptr;
    Close close = nullptr;
    PrepareV2 prepareV2 = nullptr;
    Step step = nullptr;
    Finalize finalize = nullptr;
    ColumnText columnText = nullptr;
    ColumnType columnType = nullptr;
    ColumnCount columnCount = nullptr;

    bool load() {
        if (module) {
            return true;
        }
#ifdef _WIN32
        module = LoadLibraryW(L"winsqlite3.dll");
        if (!module) {
            module = LoadLibraryA("sqlite3.dll");
        }
#else
        module = dlopen("libsqlite3.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!module) {
            module = dlopen("libsqlite3.so", RTLD_NOW | RTLD_LOCAL);
        }
#endif
        if (!module) {
            RD_LOG_DEBUG("sqlite: no sqlite3 library available");
            return false;
        }
#ifdef _WIN32
        auto symbol = [&](const char *name) {
            return GetProcAddress(reinterpret_cast<HMODULE>(module), name);
        };
#else
        auto symbol = [&](const char *name) { return dlsym(module, name); };
#endif
        openV2 = reinterpret_cast<OpenV2>(symbol("sqlite3_open_v2"));
        close = reinterpret_cast<Close>(symbol("sqlite3_close"));
        prepareV2 = reinterpret_cast<PrepareV2>(symbol("sqlite3_prepare_v2"));
        step = reinterpret_cast<Step>(symbol("sqlite3_step"));
        finalize = reinterpret_cast<Finalize>(symbol("sqlite3_finalize"));
        columnText = reinterpret_cast<ColumnText>(symbol("sqlite3_column_text"));
        columnType = reinterpret_cast<ColumnType>(symbol("sqlite3_column_type"));
        columnCount = reinterpret_cast<ColumnCount>(symbol("sqlite3_column_count"));
        if (!openV2 || !close || !prepareV2 || !step || !finalize || !columnText ||
            !columnCount) {
            RD_LOG_DEBUG("sqlite: library is missing required symbols");
            return false;
        }
        return true;
    }
};

SqliteApi &api() {
    static SqliteApi value;
    value.load();
    return value;
}

constexpr int kOpenReadOnly = 0x00000001;
constexpr int kRow = 100;
constexpr int kDone = 101;

} // namespace

Sqlite::Sqlite() = default;

Sqlite::~Sqlite() { close(); }

bool Sqlite::open(const std::string &path, bool readOnly) {
    close();
    SqliteApi &db = api();
    if (!db.module) {
        return false;
    }
    void *handle = nullptr;
    const int flags = readOnly ? kOpenReadOnly : 0;
    if (db.openV2(path.c_str(), &handle, flags, nullptr) != 0 || !handle) {
        if (handle) {
            db.close(handle);
        }
        return false;
    }
    db_ = handle;
    return true;
}

void Sqlite::close() {
    if (!db_) {
        return;
    }
    SqliteApi &db = api();
    if (db.module && db.close) {
        db.close(db_);
    }
    db_ = nullptr;
}

bool Sqlite::query(const std::string &sql, const std::function<bool(const Row &)> &row,
                   int limit) {
    if (!db_) {
        return false;
    }
    SqliteApi &db = api();
    void *statement = nullptr;
    if (db.prepareV2(db_, sql.c_str(), -1, &statement, nullptr) != 0 || !statement) {
        return false;
    }
    bool ok = true;
    int emitted = 0;
    for (;;) {
        const int result = db.step(statement);
        if (result == kDone) {
            break;
        }
        if (result != kRow) {
            ok = false;
            break;
        }
        const int columns = db.columnCount(statement);
        Row values;
        values.reserve(static_cast<size_t>(std::max(0, columns)));
        for (int i = 0; i < columns; ++i) {
            const unsigned char *text = db.columnText(statement, i);
            values.emplace_back(text ? reinterpret_cast<const char *>(text) : "");
        }
        ++emitted;
        if (!row(values)) {
            break;
        }
        if (limit > 0 && emitted >= limit) {
            break;
        }
    }
    db.finalize(statement);
    return ok;
}

} // namespace revdock
