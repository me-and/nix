#include <gtest/gtest.h>

#include "nix/store/local-store.hh"
#include "nix/store/globals.hh"
#include "nix/store/tests/libstore.hh"

// Needed for template specialisations. This is not good! When we
// overhaul how store configs work, this should be fixed.
#include "nix/util/args.hh"
#include "nix/util/config-impl.hh"
#include "nix/util/abstract-setting-to-json.hh"

namespace nix {

TEST(LocalStore, storeDir_absolutePath)
{
    std::filesystem::path storeDir =
#ifdef _WIN32
        "C:\\";
#else
        "/";
#endif
    storeDir /= "nix";
    storeDir /= "store";
    LocalStoreConfig config{"", {{"store", storeDir.string()}}};
    EXPECT_EQ(config.storeDir, storeDir.string());
}

TEST(LocalStore, storeDir_relativePath_rejected)
{
    EXPECT_THROW(LocalStoreConfig("", {{"store", (std::filesystem::path{"nix"} / "store").string()}}), UsageError);
}

TEST(LocalStore, storeDir_empty_rejected)
{
    EXPECT_THROW(LocalStoreConfig("", {{"store", ""}}), UsageError);
}

TEST(LocalStore, constructConfig_rootQueryParam)
{
#ifdef _WIN32
    constexpr std::string_view root = "C:\\foo\\bar";
#else
    constexpr std::string_view root = "/foo/bar";
#endif
    LocalStoreConfig config{
        "",
        {
            {
                "root",
                std::string{root},
            },
        },
    };

    EXPECT_EQ(config.rootDir.get(), std::optional<AbsolutePath>{std::string{root}});
}

TEST(LocalStore, constructConfig_rootPath)
{
#ifdef _WIN32
    constexpr std::string_view root = "C:\\foo\\bar";
#else
    constexpr std::string_view root = "/foo/bar";
#endif
    LocalStoreConfig config{std::string{root}, {}};

    EXPECT_EQ(config.rootDir.get(), std::optional<AbsolutePath>{std::string{root}});
}

TEST(LocalStore, constructConfig_to_string)
{
    LocalStoreConfig config{"", {}};
    EXPECT_EQ(config.getReference().to_string(), "local");
}

#ifndef _WIN32 /* Windows doesn't exactly have a notion of canonicalation for now. */

class LocalStoreGCRegistrationOrderTest : public ::testing::Test
{
protected:
    EnableExperimentalFeature feature{"gc-registration-order"};
    AutoDelete tempStoreDir{canonPath(createTempDir(), /*resolveSymlinks=*/true)};
    std::shared_ptr<LocalStoreConfig> config =
        std::make_shared<LocalStoreConfig>(tempStoreDir.path(), StoreConfig::Params{});
    std::shared_ptr<LocalStore> store = std::make_shared<LocalStore>(ref{config});

    StorePath addPath(char hash, time_t registrationTime, const StorePathSet & references = {})
    {
        StorePath path{std::string(32, hash) + "-gc-test"};
        writeFile(store->toRealPath(path), "garbage\n");
        ValidPathInfo info{path, UnkeyedValidPathInfo(*store, Hash::dummy)};
        info.registrationTime = registrationTime;
        info.references = references;
        store->registerValidPath(info);
        return path;
    }

    void setRegistrationTime(const StorePath & path, time_t registrationTime)
    {
        SQLite db(store->dbDir / "db.sqlite", {.mode = SQLiteOpenMode::NoCreate, .useWAL = true});
        SQLiteStmt stmt(db, "update ValidPaths set registrationTime = ? where path = ?");
        stmt.use().apply(registrationTime).apply(store->printStorePath(path)).exec();
    }

    GCResults collect(uint64_t maxFreed = 1, GCAction action = GCAction::gcDeleteDead)
    {
        GCOptions options;
        options.maxFreed = maxFreed;
        options.action = action;
        GCResults results;
        store->collectGarbage(options, results);
        return results;
    }

    StringSet printed(const StorePathSet & paths)
    {
        StringSet result;
        for (auto & path : paths)
            result.insert(store->printStorePath(path));
        return result;
    }

    void root(const StorePath & path)
    {
        auto rootsDir = config->stateDir.get() / "gcroots";
        createDirs(rootsDir);
        createSymlink(store->printStorePath(path), rootsDir / path.to_string());
    }
};

TEST_F(LocalStoreGCRegistrationOrderTest, oldestFirstAcrossLimitedCollections)
{
    addPath('a', 1);
    addPath('b', 1);
    addPath('c', 1);
    StorePaths paths;
    for (auto & entry : DirectoryIterator{config->realStoreDir.get()})
        if (auto path = store->maybeParseStorePath(store->storeDir + "/" + entry.path().filename().string()))
            paths.push_back(*path);
    ASSERT_EQ(paths.size(), 3);
    for (size_t i = 0; i < paths.size(); ++i)
        setRegistrationTime(paths[i], paths.size() - i);

    for (auto i = paths.rbegin(); i != paths.rend(); ++i) {
        EXPECT_EQ(collect().paths, printed({*i}));
        EXPECT_FALSE(store->isValidPath(*i));
        EXPECT_FALSE(pathExists(store->toRealPath(*i)));
    }
}

TEST_F(LocalStoreGCRegistrationOrderTest, disabledPreservesDirectoryOrder)
{
    addPath('a', 1);
    addPath('b', 1);
    StorePaths paths;
    for (auto & entry : DirectoryIterator{config->realStoreDir.get()})
        if (auto path = store->maybeParseStorePath(store->storeDir + "/" + entry.path().filename().string()))
            paths.push_back(*path);
    ASSERT_EQ(paths.size(), 2);
    setRegistrationTime(paths.front(), 2);
    setRegistrationTime(paths.back(), 1);
    auto features = experimentalFeatureSettings.experimentalFeatures.get();
    features.erase(Xp::GCRegistrationOrder);
    experimentalFeatureSettings.experimentalFeatures.assign(features);
    EXPECT_EQ(collect().paths, printed({paths.front()}));
}

TEST_F(LocalStoreGCRegistrationOrderTest, unknownTimesAndTies)
{
    auto later = addPath('a', 20);
    auto unknown = addPath('d', 10);
    auto second = addPath('c', 10);
    auto first = addPath('b', 10);
    setRegistrationTime(unknown, 0);

    for (auto & path : {unknown, first, second, later})
        EXPECT_EQ(collect().paths, printed({path}));
}

TEST_F(LocalStoreGCRegistrationOrderTest, rootsAndReferrersOverrideAge)
{
    auto rooted = addPath('a', 1);
    root(rooted);
    auto dependency = addPath('b', 2);
    auto independent = addPath('c', 3);
    auto referrer = addPath('d', 4, {dependency});

    EXPECT_EQ(collect().paths, printed({referrer}));
    EXPECT_TRUE(store->isValidPath(dependency));
    EXPECT_EQ(collect().paths, printed({dependency}));
    EXPECT_EQ(collect().paths, printed({independent}));
    EXPECT_TRUE(store->isValidPath(rooted));
    EXPECT_TRUE(pathExists(store->toRealPath(rooted)));
}

TEST_F(LocalStoreGCRegistrationOrderTest, unregisteredEntriesFirst)
{
    auto registered = addPath('a', 1);
    StorePath unregistered{"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb-unregistered"};
    writeFile(store->toRealPath(unregistered), "unregistered garbage\n");
    auto other = config->realStoreDir.get() / "not-a-store-path";
    writeFile(other, "other garbage\n");
    auto tmp = config->realStoreDir.get() / "tmp-locked";
    createDirs(tmp);
    auto fd = openDirectory(tmp, FinalSymlink::Follow);
    ASSERT_TRUE(lockFile(fd.get(), ltWrite, false));

    StringSet removed;
    for (size_t i = 0; i < 2; ++i) {
        auto result = collect();
        removed.insert(result.paths.begin(), result.paths.end());
        EXPECT_TRUE(store->isValidPath(registered));
        EXPECT_TRUE(pathExists(tmp));
    }
    auto expected = printed({unregistered});
    expected.insert(store->storeDir + "/not-a-store-path");
    EXPECT_EQ(removed, expected);
    EXPECT_EQ(collect().paths, printed({registered}));
    EXPECT_TRUE(pathExists(tmp));
}

TEST_F(LocalStoreGCRegistrationOrderTest, reportingAndZeroLimitDoNotDelete)
{
    auto path = addPath('a', 1);
    auto other = config->realStoreDir.get() / "not-a-store-path";
    writeFile(other, "garbage\n");

    EXPECT_TRUE(collect(0).paths.empty());
    EXPECT_EQ(collect(std::numeric_limits<uint64_t>::max(), GCAction::gcReturnDead).paths, printed({path}));
    EXPECT_TRUE(collect(std::numeric_limits<uint64_t>::max(), GCAction::gcReturnLive).paths.empty());
    EXPECT_TRUE(pathExists(other));
    EXPECT_TRUE(pathExists(store->toRealPath(path)));
    EXPECT_TRUE(store->isValidPath(path));
}

TEST_F(LocalStoreGCRegistrationOrderTest, explicitPathsAreNotExpanded)
{
    auto older = addPath('a', 1);
    auto requested = addPath('b', 2);
    GCOptions options;
    options.action = GCAction::gcDeleteSpecific;
    options.pathsToDelete = GCOptions::SpecificPaths{{requested}};
    GCResults results;
    store->collectGarbage(options, results);
    EXPECT_EQ(results.paths, printed({requested}));
    EXPECT_TRUE(store->isValidPath(older));
}

#endif

} // namespace nix
