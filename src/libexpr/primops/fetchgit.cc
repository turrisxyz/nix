#include "fetchgit.hh"
#include "primops.hh"
#include "eval-inline.hh"
#include "download.hh"
#include "store-api.hh"
#include "pathlocks.hh"

#include <sys/time.h>

#include <regex>

#include <nlohmann/json.hpp>

using namespace std::string_literals;

namespace nix {

GitInfo exportGit(ref<Store> store, const std::string & uri,
    const std::string & ref, const std::string & rev,
    const std::string & name)
{
    if (rev != "") {
        std::regex revRegex("^[0-9a-fA-F]{40}$");
        if (!std::regex_match(rev, revRegex))
            throw Error("invalid Git revision '%s'", rev);
    }

    Path cacheDir = getCacheDir() + "/nix/git";

    if (!pathExists(cacheDir)) {
        createDirs(cacheDir);
        runProgram("git", true, { "init", "--bare", cacheDir });
    }

    std::string localRef = hashString(htSHA256, fmt("%s-%s", uri, ref)).to_string(Base32, false);

    Path localRefFile = cacheDir + "/refs/heads/" + localRef;

    /* If the local ref is older than ‘tarball-ttl’ seconds, do a git
       fetch to update the local ref to the remote ref. */
    time_t now = time(0);
    struct stat st;
    if (stat(localRefFile.c_str(), &st) != 0 ||
        st.st_mtime < now - settings.tarballTtl)
    {
        Activity act(*logger, lvlTalkative, actUnknown, fmt("fetching Git repository '%s'", uri));

        // FIXME: git stderr messes up our progress indicator, so
        // we're using --quiet for now. Should process its stderr.
        runProgram("git", true, { "-C", cacheDir, "fetch", "--quiet", "--force", "--", uri, ref + ":" + localRef });

        struct timeval times[2];
        times[0].tv_sec = now;
        times[0].tv_usec = 0;
        times[1].tv_sec = now;
        times[1].tv_usec = 0;

        utimes(localRefFile.c_str(), times);
    }

    // FIXME: check whether rev is an ancestor of ref.
    GitInfo gitInfo;
    gitInfo.rev = rev != "" ? rev : chomp(readFile(localRefFile));
    gitInfo.shortRev = std::string(gitInfo.rev, 0, 7);

    printTalkative("using revision %s of repo '%s'", uri, gitInfo.rev);

    std::string storeLinkName = hashString(htSHA512, name + std::string("\0"s) + gitInfo.rev).to_string(Base32, false);
    Path storeLink = cacheDir + "/" + storeLinkName + ".link";
    PathLocks storeLinkLock({storeLink}, fmt("waiting for lock on '%1%'...", storeLink));

    try {
        // FIXME: doesn't handle empty lines
        auto json = nlohmann::json::parse(readFile(storeLink));

        assert(json["uri"] == uri && json["name"] == name && json["rev"] == gitInfo.rev);

        gitInfo.storePath = json["storePath"];

        if (store->isValidPath(gitInfo.storePath)) {
            gitInfo.revCount = json["revCount"];
            return gitInfo;
        }

    } catch (SysError & e) {
        if (e.errNo != ENOENT) throw;
    }

    // FIXME: should pipe this, or find some better way to extract a
    // revision.
    auto tar = runProgram("git", true, { "-C", cacheDir, "archive", gitInfo.rev });

    Path tmpDir = createTempDir();
    AutoDelete delTmpDir(tmpDir, true);

    runProgram("tar", true, { "x", "-C", tmpDir }, tar);

    gitInfo.storePath = store->addToStore(name, tmpDir);

    gitInfo.revCount = std::stoull(runProgram("git", true, { "-C", cacheDir, "rev-list", "--count", gitInfo.rev }));

    nlohmann::json json;
    json["storePath"] = gitInfo.storePath;
    json["uri"] = uri;
    json["name"] = name;
    json["rev"] = gitInfo.rev;
    json["revCount"] = gitInfo.revCount;

    writeFile(storeLink, json.dump());

    return gitInfo;
}

static void prim_fetchGit(EvalState & state, const Pos & pos, Value * * args, Value & v)
{
    std::string url;
    std::string ref = "master";
    std::string rev;
    std::string name = "source";

    state.forceValue(*args[0]);

    if (args[0]->type == tAttrs) {

        state.forceAttrs(*args[0], pos);

        for (auto & attr : *args[0]->attrs) {
            string n(attr.name);
            if (n == "url") {
                PathSet context;
                url = state.coerceToString(*attr.pos, *attr.value, context, false, false);
                if (hasPrefix(url, "/")) url = "file://" + url;
            }
            else if (n == "ref")
                ref = state.forceStringNoCtx(*attr.value, *attr.pos);
            else if (n == "rev")
                rev = state.forceStringNoCtx(*attr.value, *attr.pos);
            else if (n == "name")
                name = state.forceStringNoCtx(*attr.value, *attr.pos);
            else
                throw EvalError("unsupported argument '%s' to 'fetchGit', at %s", attr.name, *attr.pos);
        }

        if (url.empty())
            throw EvalError(format("'url' argument required, at %1%") % pos);

    } else
        url = state.forceStringNoCtx(*args[0], pos);

    // FIXME: git externals probably can be used to bypass the URI
    // whitelist. Ah well.
    state.checkURI(url);

    auto gitInfo = exportGit(state.store, url, ref, rev, name);

    state.mkAttrs(v, 8);
    mkString(*state.allocAttr(v, state.sOutPath), gitInfo.storePath, PathSet({gitInfo.storePath}));
    mkString(*state.allocAttr(v, state.symbols.create("rev")), gitInfo.rev);
    mkString(*state.allocAttr(v, state.symbols.create("shortRev")), gitInfo.shortRev);
    mkInt(*state.allocAttr(v, state.symbols.create("revCount")), gitInfo.revCount);
    v.attrs->sort();
}

static RegisterPrimOp r("fetchGit", 1, prim_fetchGit);

}
