#include <gtest/gtest.h>

#include "nix/store/local-store.hh"
#include "nix/store/globals.hh"
#include "nix/store/tests/libstore.hh"

#include "nix/util/archive.hh"
#include "nix/util/memory-source-accessor.hh"

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

class LocalStoreCanonicalisationTest : public ::testing::TestWithParam<std::tuple<HashAlgorithm>>
{
protected:
    AutoDelete tempStoreDir;
    std::shared_ptr<LocalStoreConfig> config;
    std::shared_ptr<LocalStore> store; // Inexplicably, ref doesn't have a .release() method.

    void SetUp() override
    {
        tempStoreDir = canonPath(createTempDir(), /*resolveSymlinks=*/true);
        config = std::make_shared<LocalStoreConfig>(tempStoreDir.path(), StoreConfig::Params{});
        store = std::make_shared<LocalStore>(ref{config});
    }

    void TearDown() override
    {
        tempStoreDir.deletePath();
        store.reset();
    }

    void assertCanonicalPermissions(const std::filesystem::path & path)
    {
        auto st = lstat(path);

        /* TODO: Figure out how to test xattrs properly. Maybe with posix ACLs?
           But xattrs are unavailable in the nix sandbox, so that would have to
           also run in NixOS tests? */

        auto modeTypeMaskedOut = st.st_mode & ~S_IFMT;

        /* Store mtime is always 1 sec into the epoch. */
        ASSERT_EQ(st.st_mtime, 1);

        if (S_ISLNK(st.st_mode)) {
            /* Not much to check for symlinks. */
        } else if (S_ISREG(st.st_mode)) {
            ASSERT_TRUE(modeTypeMaskedOut == 0555 || modeTypeMaskedOut == 0444);
        } else if (S_ISDIR(st.st_mode)) {
            ASSERT_EQ(modeTypeMaskedOut, 0555);
            for (const auto & p : DirectoryIterator{path})
                assertCanonicalPermissions(p.path());
        } else {
            FAIL() << fmt("unknown file type at %1%", PathFmt(path));
        }
    }

    HashAlgorithm getHashAlgo() const
    {
        return std::get<HashAlgorithm>(GetParam());
    }
};

TEST_P(LocalStoreCanonicalisationTest, flatFitsInMemory)
{
    using namespace std::string_view_literals;
    StringSource source{"very simple file"sv};
    auto sp = store->addToStoreFromDump(
        source,
        "flat",
        FileSerialisationMethod::Flat,
        /* For the purposes of this test, it doesn't really matter to vary the hashing method. */
        ContentAddressMethod::Raw::NixArchive,
        getHashAlgo(),
        {},
        NoRepair);
    assertCanonicalPermissions(store->toRealPath(sp));
    StringSink narDump;
    dumpString(source.s, narDump);
    StringSource narSource{narDump.s};
    auto sp2 = store->addToStoreFromDump(
        narSource,
        "nar",
        FileSerialisationMethod::NixArchive,
        /* For the purposes of this test, it doesn't really matter to vary the hashing method. */
        ContentAddressMethod::Raw::NixArchive,
        getHashAlgo(),
        {},
        NoRepair);
    assertCanonicalPermissions(store->toRealPath(sp2));
}

TEST_P(LocalStoreCanonicalisationTest, simpleNar)
{
    auto accessor = make_ref<MemorySourceAccessor>();
    MemorySink memorySink{*accessor};
    memorySink.createDirectory(CanonPath::root);
    memorySink.createRegularFile(CanonPath("/regular"), [](auto & crf) { crf("test"); });
    memorySink.createRegularFile(CanonPath("/executable"), [](auto & crf) {
        crf("test");
        crf.isExecutable();
    });
    memorySink.createDirectory(CanonPath("/dir"));
    memorySink.createRegularFile(CanonPath("/dir/regular"), [](auto & crf) { crf("test 2"); });
    memorySink.createRegularFile(CanonPath("/dir/executable"), [](auto & crf) {
        crf("test 2");
        crf.isExecutable();
    });
    memorySink.createSymlink(CanonPath("/symlink"), "some target");
    memorySink.createSymlink(CanonPath("/dir/symlink"), "..");
    auto source = sinkToSource([&accessor](Sink & sink) { accessor->dumpPath(CanonPath::root, sink); });
    auto sp = store->addToStoreFromDump(
        *source,
        "nar-from-dump",
        FileSerialisationMethod::NixArchive,
        /* For the purposes of this test, it doesn't really matter to vary the hashing method. */
        ContentAddressMethod::Raw::NixArchive,
        getHashAlgo(),
        {},
        NoRepair);
    assertCanonicalPermissions(store->toRealPath(sp));

    memorySink.createRegularFile(CanonPath("/other"), [](auto & crf) { crf(""); });
    auto sp2 = store->addToStoreSlow("nar-slow", {accessor}, ContentAddressMethod::Raw::NixArchive, getHashAlgo()).path;
    assertCanonicalPermissions(store->toRealPath(sp2));

    Finally restoreNarBufferSize{[oldSize = settings.getLocalSettings().narBufferSize]() {
        settings.getLocalSettings().narBufferSize.assign(oldSize);
    }};
    // So that we always spill to the file system.
    settings.getLocalSettings().narBufferSize = 0;

    memorySink.createDirectory(CanonPath("/dir/entirely-something-else"));
    StringSink narDump;
    accessor->dumpPath(CanonPath::root, narDump);
    StringSource narSource{narDump.s};
    auto sp3 = store->addToStoreFromDump(
        narSource,
        "nar-from-dump-spilled",
        FileSerialisationMethod::NixArchive,
        /* For the purposes of this test, it doesn't really matter to vary the hashing method. */
        ContentAddressMethod::Raw::NixArchive,
        getHashAlgo(),
        {},
        NoRepair);
    assertCanonicalPermissions(store->toRealPath(sp3));
}

INSTANTIATE_TEST_SUITE_P(
    LocalStoreCanonicalisation,
    LocalStoreCanonicalisationTest,
    ::testing::Combine(
        ::testing::Values(
            HashAlgorithm::SHA256,
            /* Other algorithms are included because SHA256 is special-cased in some places,
               to avoid computing narHash twice. */
            HashAlgorithm::SHA1,
            HashAlgorithm::SHA512,
            HashAlgorithm::MD5)),
    [](const ::testing::TestParamInfo<LocalStoreCanonicalisationTest::ParamType> & info) {
        return std::string(printHashAlgo(std::get<HashAlgorithm>(info.param)));
    });

/* Tests that the path info cache agrees with the database. */
class LocalStorePathInfoCacheTest : public ::testing::Test
{
protected:
    AutoDelete tempStoreDir;
    std::shared_ptr<LocalStore> store;

    void SetUp() override
    {
        tempStoreDir = canonPath(createTempDir(), /*resolveSymlinks=*/true);
        store = openStore();
    }

    void TearDown() override
    {
        store.reset();
        tempStoreDir.deletePath();
    }

    /* A fresh store object has an empty cache, so it sees what the
       database actually contains. */
    std::shared_ptr<LocalStore> openStore()
    {
        auto config = std::make_shared<LocalStoreConfig>(tempStoreDir.path(), StoreConfig::Params{});
        return std::make_shared<LocalStore>(ref{config});
    }

    ValidPathInfo makeInfo(const StorePath & path)
    {
        ValidPathInfo info{path, UnkeyedValidPathInfo(*store, Hash::dummy)};
        info.narSize = 1;
        return info;
    }

    const StorePath path{"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-foo"};
};

TEST_F(LocalStorePathInfoCacheTest, rollbackLeavesNoCachedHit)
{
    auto info = makeInfo(path);
    info.references.insert(StorePath{"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb-missing"});
    EXPECT_THROW(store->registerValidPaths({{path, info}}), InvalidPath);
    EXPECT_FALSE(openStore()->isValidPath(path));
    EXPECT_FALSE(store->isValidPath(path));
}

TEST_F(LocalStorePathInfoCacheTest, registrationClearsCachedMiss)
{
    EXPECT_THROW(store->queryPathInfo(path), InvalidPath);
    /* Registered by "another process", then re-registered by us. */
    openStore()->registerValidPaths({{path, makeInfo(path)}});
    store->registerValidPaths({{path, makeInfo(path)}});
    EXPECT_TRUE(store->isValidPath(path));
}

TEST_F(LocalStorePathInfoCacheTest, cachedInfoMatchesDatabase)
{
    store->registerValidPaths({{path, makeInfo(path)}});
    EXPECT_EQ(store->queryPathInfo(path)->registrationTime, openStore()->queryPathInfo(path)->registrationTime);
}

TEST_F(LocalStorePathInfoCacheTest, signatureUpdatesAreVisible)
{
    auto sig1 = Signature::parse(
        "key1:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA==");
    auto sig2 = Signature::parse(
        "key2:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA==");

    auto info = makeInfo(path);
    store->registerValidPaths({{path, info}});
    EXPECT_TRUE(store->queryPathInfo(path)->sigs.empty());

    info.sigs.insert(sig1);
    store->registerValidPaths({{path, info}});
    EXPECT_EQ(store->queryPathInfo(path)->sigs, std::set<Signature>{sig1});

    store->addSignatures(path, {sig2});
    EXPECT_EQ(store->queryPathInfo(path)->sigs, (std::set<Signature>{sig1, sig2}));
}

TEST_F(LocalStorePathInfoCacheTest, addSignaturesToInvalidPath)
{
    EXPECT_THROW(store->addSignatures(path, {}), InvalidPath);
    EXPECT_FALSE(store->isValidPath(path));
}

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
    auto fd = openDirectory(tmp, FinalSymlink::Follow).value();
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
