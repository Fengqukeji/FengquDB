#include "fengqudb_index.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <set>

namespace ix = fengqudb::index;
static void check(bool value, const char *why) {
    if (!value) throw std::runtime_error(why);
}
static std::string words(unsigned count, unsigned skip = UINT32_MAX) {
    std::string result;
    for (unsigned i = 0; i < count; ++i)
        if (i != skip) {
            auto n = std::to_string(i);
            result += "token" + std::string(3 - n.size(), '0') + n + ' ';
        }
    return result;
}
static std::set<std::string> ids(const ix::Result &result) {
    check(result.complete(), "query unexpectedly incomplete");
    std::set<std::string> out;
    for (const auto &hit : result.hits) out.emplace(hit.id);
    return out;
}
int main() {
    auto root = std::filesystem::current_path() /
        ("query-budget-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    bool owned = std::filesystem::create_directory(root);
    if (!owned) return 1;
    try {
        {
            fengqudb::Store store((root / "db").string());
            ix::Client index(store, "budget", "records"), empty(store, "budget", "empty");
            std::string chinese;
            for (unsigned cp = 0x4e00; cp < 0x4e00 + 80; ++cp) {
                chinese += char(0xe0 | (cp >> 12));
                chinese += char(0x80 | ((cp >> 6) & 63));
                chinese += char(0x80 | (cp & 63));
            }
            auto tx = store.transaction();
            for (const auto &[id,text] : std::vector<std::pair<std::string,std::string>>{
                     {"full", words(140)}, {"missing65", words(140,65)},
                     {"tail", "token069"}, {"missingLast", words(139)},
                     {"chinese", chinese}, {"repeat", "a"}}) {
                ix::Document d;
                d.id=id;
                d.texts={{"body",text,1}};
                index.upsert(tx,d);
            }
            check(bool(tx.commit(true)), "fixture commit");
            auto view=store.snapshot();
            ix::Query q;
            q.term_budget=1024;
            for (bool prefix : {false,true}) {
                q.prefix_last=prefix;
                q.match_all_terms=true;
                for (unsigned count : {32u,33u,63u,64u,65u,66u,70u,128u,129u,140u}) {
                    q.text=words(count);
                    std::set<std::string> want{"full"};
                    if (count<=65) want.insert("missing65");
                    if (count<140) want.insert("missingLast");
                    check(ids(index.query(view,q))==want,"AND coverage crossed word boundary incorrectly");
                    check(ids(empty.query(view,q)).empty(),"empty partition long query");
                }
                q.match_all_terms=false;
                q.text=words(70);
                check(ids(index.query(view,q)).contains("tail"),"ANY lost a term beyond bit 64");
                q.match_all_terms=true;
                q.text=chinese;
                check(ids(index.query(view,q))==std::set<std::string>{"chinese"},"long Chinese query");
            }
            q.prefix_last=false;
            q.text.clear();
            for (unsigned i=0;i<400;++i) q.text+="a ";
            q.term_budget=1;
            check(ids(index.query(view,q))==std::set<std::string>{"repeat"},"raw tokens must be deduplicated before term budget");
            q.text=words(70);
            q.term_budget=20;
            q.allow_partial=true;
            check(index.query(view,q).termination==FQIDX_TERM_LIMIT,"term budget must report explicit termination");
            q.term_budget=1024;
            q.memory_budget=4096;
            check(index.query(view,q).termination==FQIDX_MEMORY_LIMIT,"query allocations must respect memory budget");
        }
        std::filesystem::remove_all(root);
        std::cout<<"PASS query term budgets and 32/64/128-bit boundaries\n";
        return 0;
    } catch(const std::exception &e) {
        std::cerr<<e.what()<<" fixture="<<root<<'\n';
        return 1;
    }
}
