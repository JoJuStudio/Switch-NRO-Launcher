#include <switch.h>
#include <curl/curl.h>
#include <jansson.h>

#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <cstdio>       // fopen, fclose
#include <filesystem>
#include <cstdlib>      // std::exit

// -------------------- Model Types --------------------

struct Asset {
    std::string name;
    std::string url;
};

struct Release {
    std::string tag;
    std::string name;
    std::string createdAt;
    std::string commitId;
    std::string description;
    std::vector<Asset> assets;
};

// -------------------- CURL RAII Helpers --------------------

class CurlGlobal {
public:
    CurlGlobal()  { curl_global_init(CURL_GLOBAL_ALL); }
    ~CurlGlobal() { curl_global_cleanup(); }
};

class CurlEasy {
public:
    CurlEasy() {
        handle = curl_easy_init();
        if (!handle) {
            std::cerr << "CURL init failed\n";
            std::exit(1);
        }
    }
    ~CurlEasy() {
        if (handle) curl_easy_cleanup(handle);
    }

    // basic setopt
    void setopt(CURLoption opt, const char* v) {
        curl_easy_setopt(handle, opt, v);
    }
    void setopt(CURLoption opt, struct curl_slist* v) {
        curl_easy_setopt(handle, opt, v);
    }
    void setopt(CURLoption opt, long v) {
        curl_easy_setopt(handle, opt, v);
    }

    void performOrExit() {
        CURLcode res = curl_easy_perform(handle);
        if (res != CURLE_OK) {
            std::cerr << "CURL error: " << curl_easy_strerror(res) << "\n";
            std::exit(1);
        }
    }

    long getResponseCode() const {
        long code = 0;
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &code);
        return code;
    }

    CURL* getHandle() const { return handle; }

private:
    CURL* handle = nullptr;
};

// -------------------- Memory Buffer for CURL --------------------

class MemoryBuffer {
public:
    static size_t WriteCallback(void* ptr, size_t size, size_t nmemb, void* userdata) {
        auto* self = static_cast<MemoryBuffer*>(userdata);
        self->data.append(static_cast<char*>(ptr), size * nmemb);
        return size * nmemb;
    }
    std::string data;
};

// -------------------- JSON Helpers --------------------

static std::string jsonGetString(json_t* obj, const char* key) {
    if (!obj) return {};
    json_t* val = json_object_get(obj, key);
    if (json_is_string(val)) return json_string_value(val);
    return {};
}

// -------------------- Parse Releases from JSON --------------------

static std::vector<Release> parseReleases(const std::string& rawJson) {
    std::vector<Release> result;
    json_error_t err;
    json_t* root = json_loads(rawJson.c_str(), 0, &err);
    if (!root) {
        std::cerr << "JSON parse error: " << err.text << "\n";
        return result;
    }
    if (!json_is_array(root)) {
        std::cerr << "Expected JSON array\n";
        json_decref(root);
        return result;
    }

    size_t idx;
    json_t* item;
    json_array_foreach(root, idx, item) {
        Release r;
        r.tag         = jsonGetString(item, "tag_name");
        r.name        = jsonGetString(item, "name");
        r.createdAt   = jsonGetString(item, "created_at");
        r.description = jsonGetString(item, "description");
        json_t* commitObj = json_object_get(item, "commit");
        if (json_is_object(commitObj)) {
            r.commitId = jsonGetString(commitObj, "short_id");
        }

        json_t* assetsObj = json_object_get(item, "assets");
        if (json_is_object(assetsObj)) {
            json_t* links = json_object_get(assetsObj, "links");
            if (json_is_array(links)) {
                size_t ai;
                json_t* link;
                json_array_foreach(links, ai, link) {
                    Asset a;
                    a.name = jsonGetString(link, "name");
                    a.url  = jsonGetString(link, "direct_asset_url");
                    if (!a.name.empty() && !a.url.empty())
                        r.assets.push_back(a);
                }
            }
        }

        result.push_back(std::move(r));
    }

    json_decref(root);
    return result;
}

// -------------------- Fetch Releases Over Network --------------------

static std::vector<Release> fetchReleases(const std::string& apiUrl,
                                          const std::string& token)
{
    MemoryBuffer buf;
    CurlEasy curl;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("PRIVATE-TOKEN: " + token).c_str());

    curl.setopt(CURLOPT_URL, apiUrl.c_str());
    curl.setopt(CURLOPT_HTTPHEADER, headers);

    // raw handle for the function‑pointer options:
    curl_easy_setopt(curl.getHandle(), CURLOPT_WRITEFUNCTION, MemoryBuffer::WriteCallback);
    curl_easy_setopt(curl.getHandle(), CURLOPT_WRITEDATA, &buf);
    curl.setopt(CURLOPT_FOLLOWLOCATION, 1L);

    curl.performOrExit();
    long code = curl.getResponseCode();
    curl_slist_free_all(headers);

    if (code != 200) {
        std::cerr << "HTTP error: " << code << "\n";
        return {};
    }
    return parseReleases(buf.data);
}

// -------------------- UI Helpers --------------------

static int runMenu(const std::vector<std::string>& items,
                   const std::string& title)
{
    int sel = 0;
    PadState pad;
    padInitializeDefault(&pad);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);

    while (appletMainLoop()) {
        consoleClear();
        std::cout << title << "\n\n";
        for (int i = 0; i < (int)items.size(); ++i)
            std::cout << (i == sel ? "> " : "  ") << items[i] << "\n";
        consoleUpdate(nullptr);

        padUpdate(&pad);
        u64 btn = padGetButtonsDown(&pad);
        if (btn & HidNpadButton_Down)  sel = (sel + 1) % items.size();
        if (btn & HidNpadButton_Up)    sel = (sel - 1 + items.size()) % items.size();
        if (btn & HidNpadButton_A)     return sel;
        if (btn & HidNpadButton_B)     return -1;
        svcSleepThread(50'000'000ULL);
    }
    return -1;
}

static void displayRelease(const Release& r, int idx, int total) {
    consoleClear();
    std::cout
    << "Release " << (idx+1) << " of " << total << "\n\n"
    << "Tag:    " << r.tag       << "\n"
    << "Name:   " << r.name      << "\n"
    << "Commit: " << r.commitId  << "\n"
    << "Date:   " << r.createdAt << "\n\n"
    << r.description << "\n\n"
    << "Press X for assets, [+] to exit.\n";
    consoleUpdate(nullptr);
}

// -------------------- Download Asset --------------------

static void downloadAsset(const Asset& a) {
    std::atomic<bool> done{false}, canceled{false};
    std::thread td([&a,&done,&canceled](){
        CurlEasy curl;
        curl.setopt(CURLOPT_URL, a.url.c_str());
        curl.setopt(CURLOPT_FOLLOWLOCATION, 1L);

        // need raw handle for write callbacks
        CURL* h = curl.getHandle();
        std::filesystem::path out = "/sdmc/" + a.name;
        FILE* fp = std::fopen(out.c_str(), "wb");
        if (!fp) {
            std::cerr << "Failed to open " << out << "\n";
            done = true;
            return;
        }
        curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, fwrite);
        curl_easy_setopt(h, CURLOPT_WRITEDATA, fp);

        CURLcode res = curl_easy_perform(h);
        std::fclose(fp);

        if (res != CURLE_OK && !canceled) {
            std::cerr << "Download error: " << curl_easy_strerror(res) << "\n";
            std::remove(out.c_str());
        } else if (!canceled) {
            std::cout << "Downloaded to " << out << "\n";
        }
        done = true;
    });

    PadState pad;
    while (appletMainLoop() && !done) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_B) {
            canceled = true;
            break;
        }
        consoleUpdate(nullptr);
        svcSleepThread(50'000'000ULL);
    }
    td.join();

    consoleUpdate(nullptr);
    std::cout << "\nPress A to continue.\n";
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_A) break;
        consoleUpdate(nullptr);
    }
}

// -------------------- Main --------------------

int main() {
    consoleInit(nullptr);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);

    CurlGlobal curlInit;
    if (R_FAILED(socketInitializeDefault())) {
        std::cerr << "Socket initialization failed\n";
        consoleExit(nullptr);
        return 1;
    }
    nifmInitialize(NifmServiceType_User);
    nxlinkStdio();  // debug

    const std::string apiUrl =
    "https://gitlab.your-ass-is.exposed/api/v4/projects/"
    "craftcore%2Fclient-engine/releases";
        const std::string token = "glpat-u4F9t4K-4-_qRg1N7ciF";

        std::cout << "Fetching releases…\n";
        consoleUpdate(nullptr);

        auto releases = fetchReleases(apiUrl, token);
        if (releases.empty()) {
            std::cout << "No releases found.\n";
            consoleExit(nullptr);
            return 0;
        }

        int current = 0;
        displayRelease(releases[current], current, (int)releases.size());

        PadState pad;
        while (appletMainLoop()) {
            padUpdate(&pad);
            u64 btn = padGetButtonsDown(&pad);

            if (btn & HidNpadButton_Plus) break;
            if (btn & HidNpadButton_X) {
                std::vector<std::string> names;
                for (auto& a : releases[current].assets)
                    names.push_back(a.name);
                names.push_back("Back");

                int choice = runMenu(names, "Select asset:");
                if (choice >= 0 && choice < (int)names.size() - 1)
                    downloadAsset(releases[current].assets[choice]);

                displayRelease(releases[current], current, (int)releases.size());
            }
            if (btn & (HidNpadButton_Down | HidNpadButton_Right)) {
                if (current + 1 < (int)releases.size()) ++current;
                displayRelease(releases[current], current, (int)releases.size());
            }
            if (btn & (HidNpadButton_Up | HidNpadButton_Left)) {
                if (current > 0) --current;
                displayRelease(releases[current], current, (int)releases.size());
            }
            consoleUpdate(nullptr);
        }

        nifmExit();
        socketExit();
        consoleExit(nullptr);
        return 0;
}
