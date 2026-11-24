// NetCompatTests: the plugin packet compatibility rule and the wire table, with no game.
//
// Compiles the loader's own network_channel\wire_compat.h and host_rules_wire.h, which have no game or
// Windows dependency. See Tests/README.md.

#include "wire_compat.h"
#include "host_rules_wire.h"
#include "plugins/plugin_version_query.h"
#include "plugins/declared_features_text.h"
#include "plugins/server_address_text.h"
#include "plugins/plugin_catalog_text.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <initializer_list>
#include <string>
#include <vector>

using namespace WireCompat;

namespace
{
    int g_failures = 0;

    void Check(bool ok, const char* what)
    {
        std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok)
            ++g_failures;
    }

    const std::string RS(1, kRecordSep);
    const std::string US(1, kFieldSep);

    Entry Undeclared(const char* name, const char* version)
    {
        Entry e;
        e.name = name;
        e.version = version;
        return e;
    }

    Entry Declared(const char* name, const char* version, uint32_t wireId, const char* accept = "")
    {
        Entry e = Undeclared(name, version);
        e.decl = MakeDecl(wireId, accept);
        return e;
    }

    bool Compatible(const Entry& server, const Entry& client)
    {
        return IsCompatible(Evaluate(server, client));
    }

    DecodedTable Decode(const std::string& s)
    {
        return DecodeTable(s.data(), s.size());
    }

    bool SameEntry(const Entry& a, const Entry& b)
    {
        return a.name == b.name && a.version == b.version && a.decl.declared == b.decl.declared
            && a.decl.wireId == b.decl.wireId && a.decl.accept == b.decl.accept;
    }

    void TestEvaluate()
    {
        std::printf("Evaluate\n");

        Check(Evaluate(Undeclared("RT", "0.11.0"), Undeclared("RT", "0.11.0")) == Verdict::SameVersion,
            "equal strings, neither declared: same version");
        Check(Evaluate(Declared("RT", "0.11.0", 2), Declared("RT", "0.11.0", 7)) == Verdict::SameVersion,
            "equal strings win even when wire ids differ");
        Check(Evaluate(Undeclared("RT", "0.11.0"), Undeclared("RT", "0.11.1")) == Verdict::Incompatible,
            "different strings, neither declared: blocked");
        Check(Evaluate(Undeclared("RT", "0.11.0"), Undeclared("PV", "0.11.0")) == Verdict::NameMismatch,
            "equal versions but different names: never matched by name alone");
        Check(Evaluate(Declared("RT", "0.12.0", 2, "0.11.0"), Declared("PV", "0.11.0", 2)) == Verdict::NameMismatch,
            "same wire id and list but different names: name mismatch");
        Check(!IsCompatible(Verdict::NameMismatch) && !IsCompatible(Verdict::Incompatible),
            "name mismatch and incompatible are not compatible");
        Check(IsCompatible(Verdict::SameVersion) && IsCompatible(Verdict::WireIdEqual)
                && IsCompatible(Verdict::ServerListsClient) && IsCompatible(Verdict::ClientListsServer),
            "the four accepted verdicts are compatible");

        // Equal wire id.
        Check(Evaluate(Declared("RT", "0.12.0", 2), Declared("RT", "0.11.5", 2)) == Verdict::WireIdEqual,
            "both declared with equal wire id: compatible");
        Check(!Compatible(Declared("RT", "0.12.0", 2), Declared("RT", "0.11.5", 3)),
            "both declared with different wire ids: blocked");
        Check(!Compatible(Declared("RT", "0.12.0", 0), Declared("RT", "0.11.5", 0)),
            "two wire ids of 0 never match");
        Check(!Compatible(Declared("RT", "0.12.0", 2), Undeclared("RT", "0.11.5")),
            "server declared, client not, no list: blocked");
        Check(!Compatible(Undeclared("RT", "0.12.0"), Declared("RT", "0.11.5", 2)),
            "client declared, server not, no list: blocked");

        // Accept lists, each direction.
        Check(Evaluate(Declared("RT", "0.12.0", 2, "0.11.0"), Undeclared("RT", "0.11.0")) == Verdict::ServerListsClient,
            "server lists an undeclared client's version: compatible");
        Check(Evaluate(Undeclared("RT", "0.11.0"), Declared("RT", "0.12.0", 2, "0.11.0")) == Verdict::ClientListsServer,
            "client lists an undeclared server's version: compatible");
        Check(Evaluate(Declared("RT", "0.12.0", 2, "0.10.0;0.11.0"), Declared("RT", "0.11.0", 5)) == Verdict::ServerListsClient,
            "list match counts even when the wire ids differ");
        Check(!Compatible(Declared("RT", "0.12.0", 2, "0.10.0"), Undeclared("RT", "0.11.0")),
            "list that does not name the client's version: blocked");
        Check(!Compatible(Declared("RT", "0.12.0", 2, "0.11.0"), Undeclared("RT", "0.13.0")),
            "list names a version that is not the client's: blocked");
        Check(!Compatible(Declared("RT", "0.12.0", 2, "0.11"), Undeclared("RT", "0.11.0")),
            "list entry that is a prefix of the version does not match");
        Check(!Compatible(Declared("RT", "0.12.0", 2, "0.11.0.1"), Undeclared("RT", "0.11.0")),
            "version that is a prefix of the list entry does not match");
        Check(!Compatible(Declared("RT", "0.12.0", 2, "0.11.0"), Undeclared("RT", "0.11.0 ")),
            "version strings are compared exactly, trailing space included");
        Check(!Compatible(Undeclared("RT", "0.12.0"), Declared("RT", "0.11.0", 2, "0.12.0 x")),
            "a list element is whole: '0.12.0 x' does not name '0.12.0'");

        // An undeclared side's accept text never counts.
        Entry fake = Undeclared("RT", "0.12.0");
        fake.decl.accept = "0.11.0";
        Check(!Compatible(fake, Undeclared("RT", "0.11.0")), "undeclared server with stray accept text: exact string only");
        Check(!Compatible(Undeclared("RT", "0.11.0"), fake), "undeclared client with stray accept text: exact string only");

        // Reasons and text.
        Check(std::string(Reason(Verdict::WireIdEqual)) == "same wire id on both sides", "reason: wire id");
        Check(std::string(Reason(Verdict::ServerListsClient)) == "listed as compatible by the server plugin",
            "reason: server list");
        Check(std::string(Reason(Verdict::ClientListsServer)) == "listed as compatible by the client plugin",
            "reason: client list");
        Check(std::string(Reason(Verdict::SameVersion)).empty() && std::string(Reason(Verdict::Incompatible)).empty(),
            "no reason text for the other verdicts");
        Check(WireIdText(Decl{}) == "none", "wire id text: undeclared");
        Check(WireIdText(MakeDecl(7, "")) == "7", "wire id text: declared");
    }

    void TestAcceptList()
    {
        std::printf("Accept list\n");

        auto parsed = ParseAcceptList("0.10.0;0.11.0");
        Check(parsed.size() == 2 && parsed[0] == "0.10.0" && parsed[1] == "0.11.0", "two elements");
        parsed = ParseAcceptList("  0.10.0 ;\t0.11.0\r\n");
        Check(parsed.size() == 2 && parsed[0] == "0.10.0" && parsed[1] == "0.11.0", "elements are trimmed");
        Check(ParseAcceptList("").empty(), "empty list");
        Check(ParseAcceptList(";;;").empty(), "only separators");
        Check(ParseAcceptList("   ;  ; ").empty(), "only blanks");
        parsed = ParseAcceptList(";0.1.0;;0.2.0;");
        Check(parsed.size() == 2, "empty elements are ignored");
        parsed = ParseAcceptList("0.1.0;0.1.0;0.1.0");
        Check(parsed.size() == 1, "duplicates collapse");
        parsed = ParseAcceptList(std::string("ok;bad") + RS + "x;also" + US + "bad;fine");
        Check(parsed.size() == 2 && parsed[0] == "ok" && parsed[1] == "fine",
            "elements with control characters are dropped");
        parsed = ParseAcceptList("a\x01" "b;c");
        Check(parsed.size() == 1 && parsed[0] == "c", "any control character drops its element");

        std::string many;
        for (int i = 0; i < 100; ++i)
            many += "v" + std::to_string(i) + ";";
        Check(ParseAcceptList(many).size() <= kMaxAcceptItems, "item count is capped");
        Check(ParseAcceptList(std::string(kMaxAcceptLen + 1, 'x')).empty(), "oversized list is empty");
        Check(ParseAcceptList(std::string(kMaxFieldLen + 1, 'x')).empty(), "oversized single element is dropped");
        Check(ParseAcceptList(std::string(kMaxFieldLen, 'x')).size() == 1, "element at the limit is kept");

        Check(AcceptListContains("0.10.0;0.11.0", "0.11.0"), "contains: second element");
        Check(AcceptListContains("0.10.0;0.11.0", "0.10.0"), "contains: first element");
        Check(!AcceptListContains("0.10.0;0.11.0", "0.11"), "contains: not a prefix");
        Check(!AcceptListContains("0.10.0;0.11.0", "0.11.0.0"), "contains: not an extension");
        Check(!AcceptListContains("0.10.0;0.11.0", "10.0;0.11"), "contains: not a substring across elements");
        Check(!AcceptListContains("0.10.0", ""), "contains: empty version never matches");
        Check(!AcceptListContains("", "0.10.0"), "contains: empty list never matches");
        Check(!AcceptListContains(";;", ""), "contains: empty version in an empty-element list");
        Check(!AcceptListContains("0.10.0", "0.10.0;0.11.0"), "contains: a version with a separator never matches");
        Check(!AcceptListContains("V1", "v1"), "contains: case matters");

        // MakeDecl.
        bool rejected = true;
        Decl d = MakeDecl(2, " 0.10.0 ; ;0.11.0;0.10.0 ", &rejected);
        Check(d.declared && d.wireId == 2 && d.accept == "0.10.0;0.11.0" && !rejected, "MakeDecl normalizes the list");
        d = MakeDecl(3, nullptr, &rejected);
        Check(d.declared && d.wireId == 3 && d.accept.empty() && !rejected, "MakeDecl: null list");
        d = MakeDecl(3, "");
        Check(d.declared && d.accept.empty(), "MakeDecl: empty list, no out pointer");
        d = MakeDecl(4, (std::string("0.1.0") + RS + "0.2.0").c_str(), &rejected);
        Check(d.declared && d.wireId == 4 && d.accept.empty() && rejected, "MakeDecl: separator character rejects the list");
        d = MakeDecl(4, (std::string("0.1.0") + US + "0.2.0").c_str(), &rejected);
        Check(d.declared && d.accept.empty() && rejected, "MakeDecl: field separator rejects the list");
        d = MakeDecl(5, std::string(kMaxAcceptLen + 1, 'x').c_str(), &rejected);
        Check(d.declared && d.wireId == 5 && d.accept.empty() && rejected, "MakeDecl: oversized list is rejected, wire id stays");
        d = MakeDecl(0, ";;", &rejected);
        Check(d.declared && d.accept.empty() && !rejected, "MakeDecl: garbage list is empty but not rejected");
        Check(!Decl{}.declared, "a default Decl is not declared");
    }

    void TestCodec()
    {
        std::printf("Wire table codec\n");

        std::vector<Entry> in;
        in.push_back(Declared("RuptureTimer", "0.12.0", 2, "0.10.0;0.11.0"));
        in.push_back(Undeclared("ProductionViewer", "1.4.2"));
        in.push_back(Declared("Zero", "1.0.0", 0, ""));
        in.push_back(Declared("Max", "2.0.0", 0xFFFFFFFFu, "1.0.0"));
        in.push_back(Undeclared("NoVersion", ""));

        std::vector<std::string> dropped;
        const std::string wire = EncodeTable(in, &dropped);
        Check(dropped.empty(), "round trip: nothing dropped");

        const DecodedTable out = Decode(wire);
        bool same = out.present && out.entries.size() == in.size();
        for (size_t i = 0; same && i < in.size(); ++i)
            same = SameEntry(in[i], out.entries[i]);
        Check(same, "round trip: every entry comes back");
        Check(out.entries.size() == 5 && out.entries[2].decl.declared && out.entries[2].decl.wireId == 0,
            "round trip: declared wire id 0 stays declared");
        Check(out.entries.size() == 5 && !out.entries[1].decl.declared, "round trip: undeclared stays undeclared");

        // Empty table.
        const std::string empty = EncodeTable({});
        const DecodedTable e = Decode(empty);
        Check(e.present && e.entries.empty(), "header-only table is present and empty");
        Check(empty == std::string(kTableMarker) + US + "1" + RS, "header text is $wires US 1 RS");

        // Layout of one record, so a reader in another build agrees.
        const std::string one = EncodeTable({ Declared("A", "1", 9, "0") });
        Check(one == std::string("$wires") + US + "1" + RS + "A" + US + "1" + US + "9" + US + "0" + RS,
            "entry record is name US version US wire id US accept");
        const std::string undecl = EncodeTable({ Undeclared("A", "1") });
        Check(undecl == std::string("$wires") + US + "1" + RS + "A" + US + "1" + US + US + RS,
            "undeclared entry has empty wire id and accept fields");

        // Every truncation point.
        {
            bool allSafe = true;
            bool noneLostBeforeEnd = true;
            size_t lastEntries = 0;
            for (size_t cut = 0; cut <= wire.size(); ++cut)
            {
                const DecodedTable t = DecodeTable(wire.data(), cut);
                if (cut < std::string("$wires").size() + 2 + 1 && t.present)
                    allSafe = false; // header is 9 bytes, present only when complete
                if (t.entries.size() < lastEntries)
                    noneLostBeforeEnd = false;
                lastEntries = t.entries.size();
                for (const Entry& x : t.entries)
                {
                    const Entry* orig = FindEntry(in, x.name);
                    if (!orig || !SameEntry(*orig, x))
                        allSafe = false; // a truncated record must never come back altered
                }
            }
            Check(allSafe, "truncated at every byte: no crash, no altered or invented entry");
            Check(noneLostBeforeEnd, "truncated at every byte: entries only grow with more data");
            Check(DecodeTable(wire.data(), wire.size()).entries.size() == in.size(), "the full table decodes in full");
        }
        {
            const std::string headerOnlyCut = std::string("$wires") + US + "1"; // no RS
            Check(!Decode(headerOnlyCut).present, "header without its separator is not a table");
            const std::string cutRecord = wire.substr(0, wire.size() - 3);
            const DecodedTable c = Decode(cutRecord);
            Check(c.present && c.entries.size() == in.size() - 1, "a cut last record is dropped, the rest kept");
        }

        // Empty and garbage.
        Check(!DecodeTable(nullptr, 0).present, "null data: no table");
        Check(!DecodeTable("abc", 0).present, "zero length: no table");
        Check(!Decode("").present, "empty string: no table");
        Check(!Decode("hello").present, "text without separators: no table");
        Check(!Decode(RS).present, "only a separator: no table");
        Check(!Decode(RS + RS + RS).present, "only separators: no table");
        Check(!Decode(US + US + RS).present, "only field separators: no table");
        Check(!Decode(std::string("$wires") + US + "x" + RS).present, "non-numeric table version: no table");
        Check(!Decode(std::string("$wires") + US + "0" + RS).present, "table version 0: no table");
        Check(!Decode(std::string("$wires") + RS).present, "header without a version: no table");
        Check(!Decode(std::string("$wire") + US + "1" + RS).present, "wrong marker: no table");
        Check(!Decode(std::string("A") + US + "1" + US + "2" + US + "3" + RS).present, "an entry record is not a header");
        Check(Decode(std::string("$wires") + US + "2" + RS).present, "a later table version still reads as a table");
        {
            std::string junk;
            for (int i = 0; i < 256; ++i)
                junk.push_back(static_cast<char>(i));
            const DecodedTable j = Decode(junk);
            Check(!j.present || j.entries.size() <= kMaxEntries, "all byte values: no crash");
            std::string junk2 = std::string("$wires") + US + "1" + RS + junk + RS + junk;
            (void)Decode(junk2);
            Check(true, "all byte values after a header: no crash");
        }
        {
            std::string nul = std::string("$wires") + US + "1" + RS;
            nul += std::string("A\0B", 3) + US + "1" + US + US + RS;
            const DecodedTable n = Decode(nul);
            Check(n.present && n.entries.size() == 1 && n.entries[0].name.size() == 3, "an embedded NUL stays inside its field");
        }

        // Unknown and extra records and fields.
        {
            std::string t = std::string("$wires") + US + "1" + RS
                + "$future" + US + "a" + US + "b" + US + "c" + US + "d" + RS // unknown record type with four fields
                + "$note" + US + "x" + RS                                 // unknown short record
                + "RT" + US + "0.12.0" + US + "2" + US + "0.11.0" + US + "extra" + US + "more" + RS // extra fields
                + "Short" + US + "1.0" + RS                               // too few fields
                + "Empty" + RS                                            // one field
                + RS                                                      // empty record
                + "PV" + US + "1.4.2" + US + "" + RS;                     // three fields: dropped
            const DecodedTable x = Decode(t);
            Check(x.present && x.entries.size() == 1 && x.entries[0].name == "RT", "unknown, short and empty records are skipped");
            Check(x.entries.size() == 1 && x.entries[0].version == "0.12.0" && x.entries[0].decl.wireId == 2
                    && x.entries[0].decl.accept == "0.11.0",
                "extra fields on a known record are ignored");
        }
        {
            std::string t = std::string("$wires") + US + "1" + RS
                + "A" + US + "1" + US + "x2" + US + "" + RS      // bad wire id
                + "B" + US + "1" + US + "-3" + US + "" + RS      // negative
                + "C" + US + "1" + US + "4294967296" + US + "" + RS // overflow
                + "D" + US + "1" + US + "99999999999" + US + "" + RS // too long
                + "E" + US + "1" + US + " 3" + US + "" + RS      // blank in the number
                + "F" + US + "1" + US + "3" + US + "" + RS;      // fine
            const DecodedTable x = Decode(t);
            Check(x.entries.size() == 1 && x.entries[0].name == "F", "bad wire ids drop their record, others stay");
        }
        {
            std::string t = std::string("$wires") + US + "1" + RS
                + US + "1" + US + "2" + US + "" + RS                                       // empty name
                + "$x" + US + "1" + US + "2" + US + "" + RS                                // reserved name
                + std::string(kMaxFieldLen + 1, 'n') + US + "1" + US + "" + US + "" + RS   // long name
                + "L" + US + std::string(kMaxFieldLen + 1, 'v') + US + "" + US + "" + RS   // long version
                + "M" + US + "1" + US + "2" + US + std::string(kMaxAcceptLen + 1, 'a') + RS // long accept
                + "G" + US + "1" + US + "2" + US + "0.9" + RS;
            const DecodedTable x = Decode(t);
            Check(x.entries.size() == 1 && x.entries[0].name == "G", "empty, reserved and oversized fields drop their record");
        }
        {
            std::string t = std::string("$wires") + US + "1" + RS
                + "RT" + US + "1" + US + "" + US + "" + RS
                + "RT" + US + "2" + US + "5" + US + "1" + RS;
            const DecodedTable x = Decode(t);
            Check(x.entries.size() == 1 && x.entries[0].version == "1" && !x.entries[0].decl.declared,
                "a duplicate name keeps the first record");
        }
        {
            std::string t = std::string("$wires") + US + "1" + RS
                + "A" + US + "1" + US + US + "stray" + RS;
            const DecodedTable x = Decode(t);
            Check(x.entries.size() == 1 && !x.entries[0].decl.declared && x.entries[0].decl.accept.empty(),
                "accept text on an undeclared record is ignored");
        }
        {
            std::string t = std::string("$wires") + US + "1" + RS;
            for (size_t i = 0; i < kMaxEntries + 50; ++i)
                t += "P" + std::to_string(i) + US + "1" + US + US + RS;
            Check(Decode(t).entries.size() == kMaxEntries, "entry count is capped");
        }

        // Encode refuses what cannot travel.
        {
            std::vector<Entry> bad;
            bad.push_back(Undeclared("Good", "1.0"));
            bad.push_back(Undeclared((std::string("Bad") + RS + "x").c_str(), "1.0"));
            bad.push_back(Undeclared("Bad2", (std::string("1") + US + "0").c_str()));
            bad.push_back(Undeclared("$reserved", "1.0"));
            bad.push_back(Undeclared("", "1.0"));
            bad.push_back(Undeclared(std::string(kMaxFieldLen + 1, 'n').c_str(), "1.0"));
            Entry badAccept = Declared("Bad3", "1.0", 1);
            badAccept.decl.accept = std::string("a") + RS + "b";
            bad.push_back(badAccept);
            Entry bigAccept = Declared("Bad4", "1.0", 1);
            bigAccept.decl.accept = std::string(kMaxAcceptLen + 1, 'x');
            bad.push_back(bigAccept);
            bad.push_back(Declared("Fine", "2.0", 4, "1.0"));

            std::vector<std::string> dropped;
            const DecodedTable x = Decode(EncodeTable(bad, &dropped));
            Check(x.present && x.entries.size() == 2 && x.entries[0].name == "Good" && x.entries[1].name == "Fine",
                "encode leaves out entries with separators, reserved or oversized fields");
            Check(dropped.size() == 7, "encode reports every dropped entry");
            Check(Decode(EncodeTable(bad)).entries.size() == 2, "encode works without a dropped list");
        }
        {
            std::vector<Entry> lots;
            for (size_t i = 0; i < kMaxEntries + 10; ++i)
                lots.push_back(Undeclared(("P" + std::to_string(i)).c_str(), "1"));
            std::vector<std::string> dropped;
            const DecodedTable x = Decode(EncodeTable(lots, &dropped));
            Check(x.entries.size() == kMaxEntries && dropped.size() == 10, "encode caps the entry count and reports the rest");
        }
        Check(EntryEncodable(Undeclared("A", "1")) && !EntryEncodable(Undeclared("$A", "1")), "EntryEncodable");

        // Size cap: whole entries, header always kept.
        {
            std::vector<Entry> in;
            for (int i = 0; i < 20; ++i)
                in.push_back(Declared(("Plugin" + std::to_string(i)).c_str(), "1.0.0", static_cast<uint32_t>(i), "0.9.0"));
            const std::string full = EncodeTable(in);
            const size_t headerLen = EncodeTable({}).size();

            size_t truncated = 99;
            std::vector<std::string> dropped;
            const std::string roomy = EncodeTableCapped(in, full.size(), &dropped, &truncated);
            Check(roomy == full && truncated == 0 && dropped.empty(), "cap equal to the full size keeps everything");

            const std::string cut = EncodeTableCapped(in, full.size() - 1, &dropped, &truncated);
            const DecodedTable c = Decode(cut);
            Check(cut.size() <= full.size() - 1 && c.present && c.entries.size() == 19 && truncated == 1 && dropped.empty(),
                "one byte short: the last entry goes, the rest decode");

            const std::string mid = EncodeTableCapped(in, 200, nullptr, &truncated);
            const DecodedTable m = Decode(mid);
            Check(mid.size() <= 200 && m.present && !m.entries.empty() && m.entries.size() < in.size()
                    && m.entries.size() + truncated == in.size(),
                "a small cap gives fewer entries and counts the rest");
            bool prefix = true;
            for (size_t i = 0; i < m.entries.size(); ++i)
                prefix = prefix && SameEntry(in[i], m.entries[i]);
            Check(prefix, "a capped table keeps the first entries in order");

            const std::string none = EncodeTableCapped(in, 0, nullptr, &truncated);
            Check(none.size() == headerLen && Decode(none).present && Decode(none).entries.empty() && truncated == in.size(),
                "cap 0 gives the header only");
            Check(EncodeTableCapped(in, 0).size() == headerLen, "capped encode works without the counters");

            std::vector<Entry> mixed;
            mixed.push_back(Undeclared("$bad", "1"));
            mixed.push_back(Undeclared("Ok", "1"));
            dropped.clear();
            const DecodedTable x = Decode(EncodeTableCapped(mixed, 4096, &dropped, &truncated));
            Check(x.entries.size() == 1 && dropped.size() == 1 && truncated == 0, "unencodable entries are dropped, not counted as cut");
        }
    }

    void TestAckTail()
    {
        std::printf("Ack tail\n");

        // The ack's first record as an old client reads it.
        const std::string first = std::string("1.26.1") + US + "78" + RS;
        const std::string table = EncodeTable({ Declared("RT", "0.12.0", 2, "0.11.0"), Undeclared("PV", "1.4.2") });

        const std::string oldAck = first;
        Check(FindTail(oldAck.data(), oldAck.size()) == oldAck.size(), "old ack: the tail starts at the end (empty)");
        Check(!DecodeAckTail(oldAck.data(), oldAck.size()).present, "old ack: no table");

        const std::string newAck = first + table;
        const size_t at = FindTail(newAck.data(), newAck.size());
        Check(at == first.size(), "new ack: the tail starts after the first record separator");
        const DecodedTable t = DecodeAckTail(newAck.data(), newAck.size());
        Check(t.present && t.entries.size() == 2 && t.entries[0].name == "RT" && t.entries[0].decl.wireId == 2,
            "new ack: the table behind the first record decodes");
        Check(std::string(newAck.data(), at) == first, "an old reader sees exactly the old first record");

        const std::string noSep = "1.26.1" + US + "78";
        Check(FindTail(noSep.data(), noSep.size()) == kNoTail, "ack without any separator: no tail");
        Check(!DecodeAckTail(noSep.data(), noSep.size()).present, "ack without any separator: no table");
        Check(FindTail(nullptr, 0) == kNoTail && !DecodeAckTail(nullptr, 0).present, "null ack: no tail, no table");
        Check(!DecodeAckTail("", 0).present, "empty ack: no table");

        bool safe = true;
        for (size_t cut = 0; cut <= newAck.size(); ++cut)
        {
            const DecodedTable c = DecodeAckTail(newAck.data(), cut);
            if (cut < first.size() && c.present)
                safe = false;
            if (c.present && c.entries.size() > 2)
                safe = false;
        }
        Check(safe, "ack cut at every byte: no crash, never more than was sent");

        const std::string garbageTail = first + "garbage";
        Check(!DecodeAckTail(garbageTail.data(), garbageTail.size()).present, "garbage after the first record: no table");

        // Another kind of record after the table: it starts with '$' and is skipped.
        const std::string withFeature = first + table + "$feat" + US + "1" + US + "x" + RS;
        const DecodedTable f = DecodeAckTail(withFeature.data(), withFeature.size());
        Check(f.present && f.entries.size() == 2 && f.entries[1].name == "PV", "a '$' record after the table is skipped");

        // What the client does with the tail: only a table proves a new server.
        DecodedTable got;
        Check(ReadAckTail(oldAck.data(), oldAck.size(), &got) == AckTailKind::None && !got.present,
            "tail kind: an old ack has none");
        Check(ReadAckTail(noSep.data(), noSep.size()) == AckTailKind::None, "tail kind: no separator is none");
        Check(ReadAckTail(nullptr, 0) == AckTailKind::None, "tail kind: a null ack is none");
        Check(ReadAckTail(garbageTail.data(), garbageTail.size(), &got) == AckTailKind::Garbage && !got.present,
            "tail kind: bytes that are not a table are garbage");
        const std::string noHeader = first + "RT" + US + "0.12.0" + US + "2" + US + "0.11.0" + RS;
        Check(ReadAckTail(noHeader.data(), noHeader.size()) == AckTailKind::Garbage,
            "tail kind: rows without the table header are garbage");
        const std::string cutHeader = first + "$wires" + US + "1";
        Check(ReadAckTail(cutHeader.data(), cutHeader.size()) == AckTailKind::Garbage,
            "tail kind: a truncated header is garbage");
        Check(ReadAckTail(newAck.data(), newAck.size(), &got) == AckTailKind::Table && got.entries.size() == 2,
            "tail kind: a table is a table, with its rows");
        Check(ReadAckTail(newAck.data(), newAck.size()) == AckTailKind::Table, "tail kind: the table out pointer is optional");
        const std::string headerOnly = first + EncodeTable({});
        Check(ReadAckTail(headerOnly.data(), headerOnly.size(), &got) == AckTailKind::Table && got.entries.empty(),
            "tail kind: a header with no rows is a table (a new server with no plugins)");

        // The host rules record behind the table.
        const std::string rec = EncodeHostRulesRecord();
        Check(rec == std::string("$hostrules") + US + "1" + RS, "host rules record: $hostrules US 1 RS");
        const std::string withRules = first + table + rec;
        Check(TailHasHostRules(withRules.data(), withRules.size()), "host rules record behind the table is found");
        const std::string emptyWithRules = first + EncodeTable({}) + rec;
        Check(TailHasHostRules(emptyWithRules.data(), emptyWithRules.size()), "found behind a header-only table");
        const DecodedTable r = DecodeAckTail(withRules.data(), withRules.size());
        Check(r.present && r.entries.size() == 2 && r.entries[1].name == "PV",
            "an old decoder skips the host rules record and keeps every row");
        Check(ReadAckTail(withRules.data(), withRules.size(), &got) == AckTailKind::Table && got.entries.size() == 2,
            "an old tail reader still sees a table");
        Check(!TailHasHostRules(newAck.data(), newAck.size()), "a table alone has no host rules record");
        Check(!TailHasHostRules(oldAck.data(), oldAck.size()) && !TailHasHostRules(noSep.data(), noSep.size())
                && !TailHasHostRules(nullptr, 0) && !TailHasHostRules("", 0),
            "no tail, no separator, null, empty: no host rules record");
        const std::string future = first + table + "$hostrules" + US + "2" + RS;
        Check(TailHasHostRules(future.data(), future.size()), "a later record version still counts");
        const std::string zero = first + table + "$hostrules" + US + "0" + RS;
        const std::string nonNum = first + table + "$hostrules" + US + "x" + RS;
        const std::string noVer = first + table + "$hostrules" + RS;
        Check(!TailHasHostRules(zero.data(), zero.size()) && !TailHasHostRules(nonNum.data(), nonNum.size())
                && !TailHasHostRules(noVer.data(), noVer.size()),
            "version 0, a non-number or no version does not count");
        const std::string cutRec = first + table + "$hostrules" + US + "1";
        Check(!TailHasHostRules(cutRec.data(), cutRec.size()), "a record cut before its separator does not count");
        const std::string before = first + rec + table;
        Check(TailHasHostRules(before.data(), before.size()), "found wherever it sits in the tail");
        bool cutsSafe = true;
        for (size_t cut = 0; cut <= withRules.size(); ++cut)
            if (TailHasHostRules(withRules.data(), cut) && cut != withRules.size())
                cutsSafe = false;
        Check(cutsSafe, "ack cut at every byte: the record counts only when whole");
    }

    void TestClassify()
    {
        std::printf("Pair state (send, ready and receive guard)\n");

        const Entry server = Declared("RT", "0.12.0", 2, "0.11.0");
        const Entry clientOld = Undeclared("RT", "0.11.0");
        const Entry clientBad = Undeclared("RT", "0.9.0");
        Verdict why = Verdict::SameVersion;

        Check(ClassifyPair(&clientOld, server, true, &why) == PairState::Ready && why == Verdict::ServerListsClient,
            "server side: an old client in the list is ready");
        Check(ClassifyPair(&clientBad, server, true, &why) == PairState::Incompatible && why == Verdict::Incompatible,
            "server side: an unlisted client version is incompatible");
        Check(ClassifyPair(nullptr, server, true, &why) == PairState::NotReported, "server side: no entry from the client");
        Check(ClassifyPair(nullptr, server, true) == PairState::NotReported, "no verdict out pointer needed");
        Check(ClassifyPair(&server, clientOld, false, &why) == PairState::Ready && why == Verdict::ServerListsClient,
            "client side: the same pair gives the same answer");
        Check(ClassifyPair(&server, clientBad, false) == PairState::Incompatible, "client side: incompatible");
        Check(ClassifyPair(&server, clientOld, true) == PairState::Ready,
            "roles swapped: the declared side's list still names the other version");
        Check(ClassifyPair(&clientOld, Undeclared("RT", "0.11.0"), true) == PairState::Ready, "equal strings are ready");
        Check(ClassifyPair(&clientOld, Undeclared("PV", "0.11.0"), true) == PairState::Incompatible,
            "a different plugin name is never ready");

        // Guard: a client plugin packet whose plugin the manifest does not list.
        std::vector<Entry> clientTable{ clientOld };
        Check(ClassifyPair(FindEntry(clientTable, "RT"), server, true) == PairState::Ready, "guard: listed and compatible: allow");
        Check(ClassifyPair(FindEntry(clientTable, "PV"), Undeclared("PV", "1.0"), true) == PairState::NotReported,
            "guard: not listed: drop");
        Check(FindEntry(clientTable, "rt") == nullptr, "names are matched exactly");
    }

    void TestWarnOnce()
    {
        std::printf("Warn once\n");

        int a = 0;
        int b = 0;
        WarnOnce w;
        Check(w.First(&a, "RT"), "first time for a connection and plugin");
        Check(!w.First(&a, "RT"), "second time is quiet");
        Check(w.First(&a, "PV"), "another plugin on the same connection warns");
        Check(w.First(&b, "RT"), "the same plugin on another connection warns");
        Check(!w.First(&b, "RT"), "and only once there");
        Check(w.Size() == 3, "three keys held");

        w.Forget(&a);
        Check(w.Size() == 1, "forgetting a connection drops only its keys");
        Check(!w.First(&b, "RT"), "the other connection stays quiet");
        Check(w.First(&a, "RT") && w.First(&a, "PV"), "the forgotten connection warns again");

        w.Forget(&a);
        w.Forget(&a);
        w.Forget(nullptr);
        Check(w.Size() == 1, "forgetting twice or an unknown connection changes nothing");
        Check(w.First(nullptr, "RT"), "a null connection is just another key");

        w.Clear();
        Check(w.Size() == 0 && w.First(&b, "RT"), "clear forgets everything");

        // Two instances are independent, as the WARN and the INFO each keep their own.
        WarnOnce warn;
        WarnOnce info;
        Check(warn.First(&a, "RT") && info.First(&a, "RT"), "separate instances do not share keys");
        Check(!warn.First(&a, "RT") && !info.First(&a, "RT"), "each is quiet on its own");

        // A name with the separators is just a key.
        Check(w.First(&a, std::string("x") + RS + "y"), "any name works as a key");
        Check(!w.First(&a, std::string("x") + RS + "y"), "and is remembered");
    }

    void TestMirror()
    {
        std::printf("Mirror\n");
        const std::vector<Entry> server = {
            Declared("Same", "1.0", 5),
            Declared("Id", "2.0", 7),
            Declared("SrvList", "3.0", 9, "2.5;2.6"),
            Undeclared("CliList", "4.0"),
            Undeclared("Bad", "5.0"),
            Declared("OnlyServer", "1.0", 1),
            Undeclared("Plain", "6.0"),
        };
        const std::vector<Entry> local = {
            Declared("Same", "1.0", 5),
            Declared("Id", "2.1", 7),
            Declared("SrvList", "2.5", 8),
            Declared("CliList", "4.1", 3, "4.0"),
            Declared("Bad", "5.1", 1, "4.0"),
            Undeclared("OnlyLocal", "1.0"),
            Undeclared("Plain", "6.1"),
        };
        const std::vector<MirrorFinding> f = MirrorCheck(local, server);
        auto find = [&](const char* n) -> const MirrorFinding* {
            for (const MirrorFinding& m : f)
                if (m.local.name == n)
                    return &m;
            return nullptr;
        };

        Check(f.size() == 5, "only plugins on both sides with different versions are findings");
        Check(!find("Same"), "equal version strings are silent");
        Check(!find("OnlyServer") && !find("OnlyLocal"), "a name on one side only is silent");
        Check(find("Id") && find("Id")->state == PairState::Ready && find("Id")->why == Verdict::WireIdEqual,
              "equal wire ids are accepted");
        Check(find("SrvList") && find("SrvList")->state == PairState::Ready
                  && find("SrvList")->why == Verdict::ServerListsClient,
              "the server lists the client's version");
        Check(find("CliList") && find("CliList")->state == PairState::Ready
                  && find("CliList")->why == Verdict::ClientListsServer,
              "the client lists the server's version");
        Check(find("Bad") && find("Bad")->state == PairState::Incompatible && find("Bad")->why == Verdict::Incompatible,
              "different wire ids and no list is blocked");
        Check(find("Plain") && find("Plain")->state == PairState::Incompatible,
              "undeclared on both sides with different versions is blocked");
        Check(find("Bad") && find("Bad")->server.version == "5.0" && find("Bad")->local.version == "5.1",
              "a finding carries both versions");
        Check(MirrorCheck(local, {}).empty() && MirrorCheck({}, server).empty(), "an empty side has no findings");

        const Decl a = MakeDecl(7, "");
        const Decl b = MakeDecl(8, "");
        Check(WireIdsNote(a, b) == " (wire id 7 on the server, 8 on the client)", "ids note names both ids");
        Check(WireIdsNote(Decl{}, b).empty() && WireIdsNote(a, Decl{}).empty(), "ids note is empty unless both declared");

        const std::vector<Entry> mine = { Undeclared("B", "2"), Undeclared("A", "1.0"), Undeclared("C", "3") };
        Check(DescribePlugins({ "B", "A", "B", "Ghost" }, mine) == "A 1.0, B 2",
              "plugin list is sorted, deduplicated and skips unknown names");
        Check(DescribePlugins({}, mine).empty() && DescribePlugins({ "Ghost" }, mine).empty(), "nothing to list gives empty text");
    }
}

namespace HW = HostRulesWire;

namespace
{
    std::string Rec(std::initializer_list<std::string> fields)
    {
        std::string s;
        bool first = true;
        for (const std::string& f : fields)
        {
            if (!first)
                s += HW::kFieldSep;
            s += f;
            first = false;
        }
        s += HW::kRecordSep;
        return s;
    }

    HW::Status DecF(const std::string& s, HW::FeatPayload* p = nullptr)
    {
        HW::FeatPayload tmp;
        return HW::DecodeFeat(s.data(), s.size(), p ? p : &tmp);
    }

    HW::Status DecR(const std::string& s, HW::RulesSet* r = nullptr)
    {
        HW::RulesSet tmp;
        return HW::DecodeRules(s.data(), s.size(), r ? r : &tmp);
    }

    const std::string kFeatHead = Rec({ "$loader", "v1.27.0", "79" });
    const std::string kRulesHead = Rec({ "$rules", "1", "1", "0", "" });

    // The reader as it was before "$game": every '$' row is skipped, nothing is read from it.
    HW::Status OldDecodeFeat(const std::string& s, HW::FeatPayload* out)
    {
        std::vector<HW::detail::Fields> recs;
        const HW::Status st = HW::detail::SplitRecords(s.data(), s.size(), &recs);
        if (st != HW::Status::Ok)
            return st;
        uint32_t iface = 0;
        const HW::detail::Fields& h = recs[0];
        if (h.size() < 3 || h[0] != HW::kLoaderMarker || h[1].empty() || !HW::detail::ParseUint(h[2], &iface))
            return HW::Status::BadHeader;
        HW::FeatPayload p;
        p.buildTag.assign(h[1]);
        p.interfaceVersion = iface;
        for (size_t i = 1; i < recs.size(); ++i)
        {
            const HW::detail::Fields& r = recs[i];
            if (!r[0].empty() && r[0][0] == '$')
                continue;
            bool on = false;
            if (r.size() < 4 || r[0].empty() || r[1].empty() || !HW::detail::ParseFlag(r[3], &on))
                return HW::Status::BadRecord;
            p.features.push_back({ std::string(r[0]), std::string(r[1]), std::string(r[2]), on });
        }
        *out = std::move(p);
        return HW::Status::Ok;
    }

    void TestHostGameBuild()
    {
        std::printf("Host rules: $game record in $ML:feat\n");
        HW::FeatPayload in;
        in.buildTag = "v1.26.1-dev.14";
        in.interfaceVersion = 79;
        in.gameCL = 130045;
        in.gameImageStamp = 0x6A1B2C3Du;
        in.gameImageSize = 0x0123ABCDu;
        in.features = { { "BetterCheats", "movement", "Movement", true } };
        const std::string wire = HW::EncodeFeat(in);
        Check(wire.find(Rec({ "$game", "130045", "6a1b2c3d", "123abcd" })) == Rec({ "$loader", "v1.26.1-dev.14", "79" }).size(),
            "the $game record follows the header: decimal CL, hex stamp, hex size");
        HW::FeatPayload out;
        Check(DecF(wire, &out) == HW::Status::Ok && out.gameCL == 130045 && out.gameImageStamp == 0x6A1B2C3Du
                && out.gameImageSize == 0x0123ABCDu && out.features.size() == 1 && out.buildTag == in.buildTag,
            "round trip with $game keeps the build and the features");

        in.gameCL = 0;
        const std::string plain = HW::EncodeFeat(in);
        Check(plain.find("$game") == std::string::npos, "CL 0: no $game record is written");
        Check(DecF(plain, &out) == HW::Status::Ok && out.gameCL == 0 && out.features.size() == 1, "round trip without $game: CL stays 0");

        // An old client's payload through the new reader: nothing known, nothing refused.
        Check(DecF(kFeatHead + Rec({ "P", "i", "t", "1" }), &out) == HW::Status::Ok && out.gameCL == 0 && out.features.size() == 1,
            "an old-format payload decodes with no game build");

        // A new client's payload through the unchanged old reader: the record is skipped.
        HW::FeatPayload old;
        Check(OldDecodeFeat(wire, &old) == HW::Status::Ok && old.features.size() == 1 && old.features[0].id == "movement"
                && old.buildTag == in.buildTag,
            "a payload with $game decodes through the old skip-$ reader");

        // A bad $game record is ignored; the feature rows still decode.
        const std::string row = Rec({ "P", "i", "t", "1" });
        const char* badCL[] = { "", "x", "0", "4294967296", "-5" };
        for (const char* cl : badCL)
            Check(DecF(kFeatHead + Rec({ "$game", cl, "1", "2" }) + row, &out) == HW::Status::Ok && out.gameCL == 0 && out.features.size() == 1,
                "a $game record with a bad CL is ignored");
        Check(DecF(kFeatHead + Rec({ "$game", "5", "zz", "2" }) + row, &out) == HW::Status::Ok && out.gameCL == 0 && out.features.size() == 1,
            "a $game record with a bad stamp is ignored");
        Check(DecF(kFeatHead + Rec({ "$game", "5", "1", "" }) + row, &out) == HW::Status::Ok && out.gameCL == 0 && out.features.size() == 1,
            "a $game record with an empty size is ignored");
        Check(DecF(kFeatHead + Rec({ "$game", "5", "123456789", "2" }) + row, &out) == HW::Status::Ok && out.gameCL == 0,
            "a $game record with a stamp over 8 digits is ignored");
        Check(DecF(kFeatHead + Rec({ "$game", "5" }) + row, &out) == HW::Status::Ok && out.gameCL == 0 && out.features.size() == 1,
            "a $game record with too few fields is ignored");
        Check(DecF(kFeatHead + Rec({ "$game", "5", "A", "B" }) + row, &out) == HW::Status::Ok && out.gameCL == 5 && out.gameImageStamp == 0xA,
            "hex digits are read in either case");
        Check(DecF(kFeatHead + Rec({ "$game", "5", "1", "2" }) + Rec({ "$game", "6", "1", "2" }) + row, &out) == HW::Status::Ok && out.gameCL == 6,
            "with two $game records the last good one is kept");

        // The decision.
        Check(!HW::GameBuildMismatch(130045, 130045), "same build is no mismatch");
        Check(HW::GameBuildMismatch(130045, 127004), "different builds are a mismatch");
        Check(HW::GameBuildMismatch(127004, 130045), "different builds are a mismatch the other way");
        Check(!HW::GameBuildMismatch(0, 127004), "client build unknown (old loader): no mismatch");
        Check(!HW::GameBuildMismatch(130045, 0), "server build unknown: no mismatch");
        Check(!HW::GameBuildMismatch(0, 0), "both unknown: no mismatch");
    }

    void TestHostFeat()
    {
        std::printf("Host rules: $ML:feat\n");
        HW::FeatPayload in;
        in.buildTag = "v1.26.1-dev.7";
        in.interfaceVersion = 78;
        in.features = { { "BetterCheats", "movement", "Movement", true },
                        { "BetterCheats", "item-spawner", "Item spawner", false },
                        { "BetterDrone", "drone-limits", "Drone limits \xC3\xA9", true } };
        HW::EncodeStats st;
        const std::string wire = HW::EncodeFeat(in, &st);
        Check(st.dropped == 0 && st.truncated == 0, "encode drops and cuts nothing from a clean set");
        Check(wire.rfind(Rec({ "$loader", "v1.26.1-dev.7", "78" }), 0) == 0, "first record is $loader tag and interface version");
        HW::FeatPayload out;
        Check(DecF(wire, &out) == HW::Status::Ok, "round trip decodes");
        Check(out.buildTag == in.buildTag && out.interfaceVersion == 78 && out.features.size() == 3, "header and count survive");
        bool same = out.features.size() == 3;
        for (size_t i = 0; same && i < 3; ++i)
            same = out.features[i].plugin == in.features[i].plugin && out.features[i].id == in.features[i].id
                && out.features[i].title == in.features[i].title && out.features[i].on == in.features[i].on;
        Check(same, "every feature survives: plugin, id, title (UTF-8), state");

        HW::FeatPayload none;
        none.buildTag = "dev";
        Check(DecF(HW::EncodeFeat(none), &out) == HW::Status::Ok && out.features.empty() && out.buildTag == "dev",
            "no features is a header only");

        // Writer: what cannot travel.
        HW::FeatPayload bad;
        bad.buildTag = std::string("a") + HW::kFieldSep + "b";
        bad.features = { { "", "x", "t", true }, { "$P", "x", "t", true }, { "P", "", "t", true }, { "P", "*", "t", true },
                         { "P\x01", "x", "t", true }, { "P", "x\x1f", "t", true }, { std::string(129, 'p'), "x", "t", true },
                         { "Good", "id", std::string("T\x1f") + "i\ntle", true } };
        const std::string w2 = HW::EncodeFeat(bad, &st);
        Check(st.dropped == 7, "encode drops empty, reserved, wildcard, control and oversize plugin or id");
        Check(DecF(w2, &out) == HW::Status::Ok && out.buildTag == "dev" && out.features.size() == 1
                && out.features[0].title == "T i tle",
            "a bad tag goes out as dev; a title with control characters is made safe, not dropped");

        // Reader: hostile input.
        Check(DecF("") == HW::Status::Empty, "empty input");
        Check(DecF(wire.substr(0, wire.size() - 1)) == HW::Status::Truncated, "last record cut before its separator");
        Check(DecF(kFeatHead + "Pl" + HW::kFieldSep + "id") == HW::Status::Truncated, "unfinished tail record");
        bool prefixOk = true;
        for (size_t n = 0; n < wire.size(); ++n)
            if (n == 0 || wire[n - 1] != HW::kRecordSep)
                prefixOk = prefixOk && DecF(wire.substr(0, n)) != HW::Status::Ok;
        Check(prefixOk, "no prefix that ends mid-record decodes (every cut checked)");
        Check(DecF(Rec({ "$loaderx", "v1", "1" })) == HW::Status::BadHeader, "wrong marker");
        Check(DecF(Rec({ "$loader", "v1" })) == HW::Status::BadHeader, "header with too few fields");
        Check(DecF(Rec({ "$loader", "", "1" })) == HW::Status::BadHeader, "empty build tag");
        Check(DecF(Rec({ "$loader", "v1", "x" })) == HW::Status::BadHeader, "interface version not a number");
        Check(DecF(Rec({ "$loader", "v1", "4294967296" })) == HW::Status::BadHeader, "interface version over 32 bits");
        Check(DecF(Rec({ "$loader", std::string(65, 'v'), "1" })) == HW::Status::FieldTooLong, "build tag too long");
        Check(DecF(Rec({ "P", "i", "t", "1" })) == HW::Status::BadHeader, "no $loader record first");
        Check(DecF(kFeatHead + Rec({ "P", "i", "t" })) == HW::Status::BadRecord, "row with three fields");
        Check(DecF(kFeatHead + Rec({ "P", "i", "t", "2" })) == HW::Status::BadRecord, "state 2");
        Check(DecF(kFeatHead + Rec({ "P", "i", "t", "" })) == HW::Status::BadRecord, "empty state");
        Check(DecF(kFeatHead + Rec({ "", "i", "t", "1" })) == HW::Status::BadRecord, "empty plugin");
        Check(DecF(kFeatHead + Rec({ "P", "", "t", "1" })) == HW::Status::BadRecord, "empty id");
        Check(DecF(kFeatHead + Rec({ "P", "*", "t", "1" })) == HW::Status::BadRecord, "a feature cannot be named *");
        Check(DecF(kFeatHead + std::string(1, HW::kRecordSep)) == HW::Status::BadRecord, "empty record");
        Check(DecF(kFeatHead + Rec({ std::string(129, 'p'), "i", "t", "1" })) == HW::Status::FieldTooLong, "plugin too long");
        Check(DecF(kFeatHead + Rec({ "P", std::string(129, 'i'), "t", "1" })) == HW::Status::FieldTooLong, "id too long");
        Check(DecF(kFeatHead + Rec({ "P", "i", std::string(257, 't'), "1" })) == HW::Status::FieldTooLong, "title too long");
        Check(DecF(kFeatHead + Rec({ "P", "i", std::string(256, 't'), "1" })) == HW::Status::Ok, "title at the limit is fine");
        Check(DecF(kFeatHead + Rec({ "P", "i", "t", "1", "2", "3", "4", "5", "6" })) == HW::Status::BadRecord,
            "more than the allowed fields in a row");
        Check(DecF(kFeatHead + Rec({ "P", "i", "t", "1", "extra" })) == HW::Status::Ok, "an extra field is ignored");
        for (char c : std::string("\x01\x02\t\n\r\x1b\x7f"))
        {
            const std::string t = std::string("a") + c + "b";
            Check(DecF(kFeatHead + Rec({ "P", "i", t, "1" })) == HW::Status::ControlChar, "control character in a title is refused");
            Check(DecF(Rec({ "$loader", t, "1" })) == HW::Status::ControlChar, "control character in the build tag is refused");
        }
        {
            std::string nul = "a";
            nul += '\0';
            nul += "b";
            Check(DecF(kFeatHead + Rec({ "P", nul, "t", "1" })) == HW::Status::ControlChar, "NUL inside a field is refused");
        }
        Check(DecF(kFeatHead + Rec({ "P", "i", "a" + std::string(1, HW::kFieldSep) + "b", "1" })) == HW::Status::BadRecord,
            "a separator inside a title makes the state field 'b': refused");
        Check(DecF(kFeatHead + Rec({ "$future", "x", "y" })) == HW::Status::Ok, "an unknown $ record is skipped");
        Check(DecF(kFeatHead + Rec({ "P", "i", "t", "1" }) + Rec({ "p", "i", "t2", "0" }) + Rec({ "P", "j", "t", "0" }), &out) == HW::Status::Ok
                && out.features.size() == 2 && out.features[0].title == "t" && out.features[0].on,
            "a repeat of plugin (any case) and id is skipped, the first stays");

        // Failure leaves the output alone.
        HW::FeatPayload keep;
        keep.buildTag = "keep";
        DecF(kFeatHead + Rec({ "P", "i", "t", "9" }), &keep);
        Check(keep.buildTag == "keep", "a refused payload leaves the output untouched");

        // Record count and size.
        std::string many = kFeatHead;
        for (size_t i = 0; i + 1 < HW::kMaxRecords; ++i)
            many += Rec({ "P", "id" + std::to_string(i), "t", "1" });
        Check(DecF(many, &out) == HW::Status::Ok && out.features.size() == HW::kMaxRecords - 1, "exactly the record limit decodes");
        Check(DecF(many + Rec({ "P", "last", "t", "1" })) == HW::Status::TooManyRecords, "one record over the limit");
        std::string huge = kFeatHead + Rec({ "P", "i", "t", "1" });
        huge.append(HW::kMaxPayloadBytes, 'x');
        Check(DecF(huge) == HW::Status::TooLarge, "payload over the byte limit");

        // 100+ features.
        HW::FeatPayload big;
        big.buildTag = "v1.27.0";
        for (int i = 0; i < 300; ++i)
            big.features.push_back({ "Plugin" + std::to_string(i % 7), "feature-" + std::to_string(i), "Title " + std::to_string(i), (i % 3) != 0 });
        const std::string bw = HW::EncodeFeat(big, &st);
        Check(DecF(bw, &out) == HW::Status::Ok && out.features.size() == 300 && st.truncated == 0, "300 features round trip");
        Check(out.features[299].id == "feature-299" && out.features[299].on == true && out.features[297].on == false, "order and state of the last rows");

        // Encode limits.
        HW::FeatPayload over;
        over.buildTag = "v1";
        for (size_t i = 0; i < HW::kMaxRecords + 10; ++i)
            over.features.push_back({ "P", "id" + std::to_string(i), "t", true });
        const std::string ow = HW::EncodeFeat(over, &st);
        Check(st.truncated == 11 && DecF(ow, &out) == HW::Status::Ok && out.features.size() == HW::kMaxRecords - 1,
            "past the record limit the writer cuts and counts, and the reader still accepts the result");
        HW::FeatPayload fat;
        fat.buildTag = "v1";
        for (int i = 0; i < 1000; ++i)
            fat.features.push_back({ std::string(100, 'p'), std::string(100, 'i') + std::to_string(i), std::string(200, 't'), true });
        const std::string fw = HW::EncodeFeat(fat, &st);
        Check(fw.size() <= HW::kMaxPayloadBytes && st.truncated > 0 && DecF(fw, &out) == HW::Status::Ok, "past the size limit the writer cuts, and the result decodes");
    }

    void TestHostRules()
    {
        std::printf("Host rules: $ML:rules\n");
        HW::RulesSet in;
        in.defaultAllow = false;
        in.onDenied = 0;
        in.message = "This server restricts some mods. See #rules.";
        in.deny = { { "BetterCheats", "movement" }, { "BetterCheats", "inventory" }, { "SomeOtherMod", "*" } };
        in.allow = { { "BetterCheats", "*" }, { "BetterDrone", "map-reveal" } };
        HW::EncodeStats st;
        const std::string wire = HW::EncodeRules(in, &st);
        Check(st.dropped == 0 && st.truncated == 0, "encode drops and cuts nothing from a clean set");
        HW::RulesSet out;
        Check(DecR(wire, &out) == HW::Status::Ok, "round trip decodes");
        Check(!out.defaultAllow && out.onDenied == 0 && out.message == in.message, "header: default, mode and message survive");
        Check(out.deny.size() == 3 && HostRules::Contains(out.deny, "BetterCheats", "movement")
                && HostRules::Contains(out.deny, "bettercheats", "inventory") && HostRules::Contains(out.deny, "SomeOtherMod", "*"),
            "deny rows land in the L1 deny set (plugin name any case, id exact, * kept)");
        Check(out.allow.size() == 2 && HostRules::Contains(out.allow, "BetterCheats", "*") && HostRules::Contains(out.allow, "BetterDrone", "map-reveal"),
            "allow rows are carried");

        HW::RulesSet empty;
        Check(DecR(HW::EncodeRules(empty), &out) == HW::Status::Ok && out.defaultAllow && out.deny.empty() && out.allow.empty(),
            "an empty set is a header only: lifts every rule");
        Check(HW::EncodeRules(empty) == Rec({ "$rules", "1", "1", "0", "" }), "the header bytes are as documented");

        // Writer rules.
        HW::RulesSet w;
        w.message = std::string("line one\nline\ttwo ") + HW::kRecordSep + "x" + std::string(300, 'y');
        w.deny = { { "P", "a" }, { "p", "a" }, { "P", "b" }, { "", "x" }, { "$R", "x" }, { "Q", "" }, { "Q\x02", "x" } };
        w.allow = { { "P", "a" }, { "P", "c" } };
        const std::string ww = HW::EncodeRules(w, &st);
        Check(st.dropped == 2, "rows with a reserved or control-character plugin or id are dropped and counted (empty rows vanish in Normalize)");
        Check(DecR(ww, &out) == HW::Status::Ok && out.deny.size() == 2 && out.allow.size() == 1
                && HostRules::Contains(out.allow, "P", "c"),
            "repeats collapse and an allow row that a deny row names is left out: deny wins");
        Check(out.message.size() <= HW::kMaxMessageLen && out.message.find('\n') == std::string::npos
                && out.message.find('\t') == std::string::npos && out.message.compare(0, 8, "line one") == 0,
            "the message is cut to the limit and control characters become spaces");
        Check(HW::SanitizeText(std::string(199, 'a') + "\xC3\xA9" + "bc", 200) == std::string(199, 'a'), "a cut never splits a UTF-8 character");
        Check(HW::SanitizeText("  hi  ", 10) == "hi" && HW::SanitizeText("", 10).empty(), "ends are trimmed");

        // Reader: hostile input.
        Check(DecR("") == HW::Status::Empty, "empty input");
        Check(DecR(wire.substr(0, wire.size() - 1)) == HW::Status::Truncated, "last record cut before its separator");
        Check(DecR(kRulesHead + "P") == HW::Status::Truncated, "unfinished row");
        Check(DecR(Rec({ "$rule", "1", "1", "0", "" })) == HW::Status::BadHeader, "wrong marker");
        Check(DecR(Rec({ "$rules", "0", "1", "0", "" })) == HW::Status::BadHeader, "version 0");
        Check(DecR(Rec({ "$rules", "1", "2", "0", "" })) == HW::Status::BadHeader, "default must be 0 or 1");
        Check(DecR(Rec({ "$rules", "1", "1", "256", "" })) == HW::Status::BadHeader, "on-denied mode over 255");
        Check(DecR(Rec({ "$rules", "1", "1", "x", "" })) == HW::Status::BadHeader, "on-denied mode not a number");
        Check(DecR(Rec({ "$rules", "1", "1", "0" })) == HW::Status::BadHeader, "header without the message field");
        Check(DecR(Rec({ "$rules", "1", "1", "0", std::string(201, 'm') })) == HW::Status::FieldTooLong, "message over the limit");
        Check(DecR(Rec({ "$rules", "1", "1", "0", std::string(200, 'm') })) == HW::Status::Ok, "message at the limit is fine");
        Check(DecR(Rec({ "$rules", "2", "1", "0", "", "future" })) == HW::Status::Ok, "a newer version with extra fields is read as version 1");
        Check(DecR(Rec({ "$rules", "1", "1", "0", std::string("a\x01") + "b" })) == HW::Status::ControlChar, "control character in the message");
        Check(DecR(kRulesHead + Rec({ "P", "i" })) == HW::Status::BadRecord, "row with two fields");
        Check(DecR(kRulesHead + Rec({ "P", "i", "2" })) == HW::Status::BadRecord, "state 2");
        Check(DecR(kRulesHead + Rec({ "", "i", "0" })) == HW::Status::BadRecord, "empty plugin");
        Check(DecR(kRulesHead + Rec({ "P", "", "0" })) == HW::Status::BadRecord, "empty id");
        Check(DecR(kRulesHead + Rec({ std::string(129, 'p'), "i", "0" })) == HW::Status::FieldTooLong, "plugin too long");
        Check(DecR(kRulesHead + Rec({ "P", std::string(129, 'i'), "0" })) == HW::Status::FieldTooLong, "id too long");
        Check(DecR(kRulesHead + Rec({ "P", "i\x1b", "0" })) == HW::Status::ControlChar, "control character in an id");
        Check(DecR(kRulesHead + Rec({ "P", "a" + std::string(1, HW::kFieldSep) + "b", "0" })) == HW::Status::BadRecord,
            "a separator inside an id shifts the state field: refused");
        Check(DecR(kRulesHead + Rec({ "$future", "x", "0" })) == HW::Status::Ok, "an unknown $ record is skipped");
        Check(DecR(kRulesHead + Rec({ "P", "i", "0", "1", "2", "3", "4", "5", "6" })) == HW::Status::BadRecord, "too many fields in a row");
        HW::RulesSet keep;
        keep.message = "keep";
        DecR(kRulesHead + Rec({ "P", "i", "7" }), &keep);
        Check(keep.message == "keep", "a refused payload leaves the output untouched");
        Check(DecR(kRulesHead + Rec({ "P", "i", "0" }) + Rec({ "p", "i", "0" }) + Rec({ "p", "i", "1" }), &out) == HW::Status::Ok
                && out.deny.size() == 1 && out.allow.empty(),
            "a clash in the data is deny wins");

        // 100+ rows and the limits.
        HW::RulesSet big;
        big.defaultAllow = true;
        for (int i = 0; i < 400; ++i)
            ((i % 4) ? big.deny : big.allow).push_back({ "Plugin" + std::to_string(i % 11), "feature-" + std::to_string(i) });
        big.deny.push_back({ "Whole", "*" });
        const std::string bw = HW::EncodeRules(big, &st);
        Check(DecR(bw, &out) == HW::Status::Ok && out.deny.size() == 301 && out.allow.size() == 100 && st.truncated == 0, "400 rows round trip");
        std::string many = kRulesHead;
        for (size_t i = 0; i + 1 < HW::kMaxRecords; ++i)
            many += Rec({ "P", "id" + std::to_string(i), "0" });
        Check(DecR(many, &out) == HW::Status::Ok && out.deny.size() == HW::kMaxRecords - 1, "exactly the record limit decodes");
        Check(DecR(many + Rec({ "P", "last", "0" })) == HW::Status::TooManyRecords, "one record over the limit");
        HW::RulesSet over;
        for (size_t i = 0; i < HW::kMaxRecords + 5; ++i)
            over.deny.push_back({ "P", "id" + std::to_string(i) });
        const std::string ow = HW::EncodeRules(over, &st);
        Check(st.truncated == 6 && DecR(ow, &out) == HW::Status::Ok && out.deny.size() == HW::kMaxRecords - 1,
            "past the record limit the writer cuts and counts, and the reader accepts the result");
    }

    void TestHostPlan()
    {
        std::printf("Host rules: fragmentation plan\n");
        HW::FeatPayload p;
        p.buildTag = "v1.27.0";
        p.interfaceVersion = 79;
        const std::string small = HW::EncodeFeat(p);
        const HW::SendPlan a = HW::PlanSend(small.size(), HW::kTagFeat);
        const std::string expect = "[MOD:$ML:feat:" + std::to_string(small.size()) + ":]";
        Check(a.single && a.sendable && a.chunkCount == 1 && a.envelopeBytes == expect.size() + 4 * ((small.size() + 2) / 3),
            "a small payload is one bunch; the envelope size is the header plus base64");
        // The largest payload that stays in one bunch.
        size_t n = 0;
        while (HW::PlanSend(n + 1, HW::kTagRules).single)
            ++n;
        Check(n > 600 && n < 760 && HW::PlanSend(n, HW::kTagRules).envelopeBytes <= HW::kBunchBytes
                && HW::PlanSend(n + 1, HW::kTagRules).envelopeBytes > HW::kBunchBytes,
            "the single-bunch edge sits where the envelope reaches 1024 bytes");
        const HW::SendPlan b = HW::PlanSend(n + 1, HW::kTagRules);
        Check(!b.single && b.chunkBudget == 1024 - 22 - 3 && b.chunkCount == 2 && b.sendable, "one byte over is two chunks of 999");
        // Chunks times budget covers the envelope, one fewer does not.
        const HW::SendPlan c = HW::PlanSend(30000, HW::kTagRules);
        Check(c.chunkCount * c.chunkBudget >= c.envelopeBytes && (c.chunkCount - 1) * c.chunkBudget < c.envelopeBytes, "chunk count is the ceiling");
        // The real chunker agrees.
        std::string env(c.envelopeBytes, 'x');
        const auto frames = Fragmentation::BuildChunks(1, "$ML", reinterpret_cast<const uint8_t*>(env.data()), env.size(), c.chunkBudget);
        Check(frames.size() == c.chunkCount && frames[0].size() <= HW::kBunchBytes, "BuildChunks gives the planned count and each frame fits a bunch");
        // The largest payload either message can be.
        const HW::SendPlan m = HW::PlanSend(HW::kMaxPayloadBytes, HW::kTagFeat);
        Check(m.sendable && m.chunkCount < 1000, "a maximum size payload is sendable");
        Check(!HW::PlanSend(10u * 1024u * 1024u, HW::kTagRules).sendable, "past the reassembler's byte limit is not sendable");
        Check(!HW::PlanSend(5000, HW::kTagRules, 25).sendable, "a bunch with no room after the chunk header is not sendable");
        Check(HW::PlanSend(0, HW::kTagFeat).single, "an empty payload is one bunch");
        HW::FeatPayload big;
        big.buildTag = "v1.27.0";
        for (int i = 0; i < 120; ++i)
            big.features.push_back({ "BetterCheats", "feature-" + std::to_string(i), "A feature title", true });
        const HW::SendPlan d = HW::PlanSend(HW::EncodeFeat(big).size(), HW::kTagFeat);
        Check(!d.single && d.sendable && d.chunkCount > 5, "a 120 feature declaration is fragmented");
    }

    // The client re-sends $ML:feat only when the encoded bytes change, so the
    // same catalog must encode to the same bytes and a flipped state must not.
    void TestHostFeatStable()
    {
        std::printf("Host rules: $ML:feat payload equality\n");
        HW::FeatPayload a;
        a.buildTag = "v1.27.0";
        a.interfaceVersion = 79;
        a.features = { { "P", "one", "One", true }, { "P", "two", "Two", false } };
        HW::FeatPayload b = a;
        Check(HW::EncodeFeat(a) == HW::EncodeFeat(b), "the same catalog encodes to the same bytes");
        b.features[1].on = true;
        Check(HW::EncodeFeat(a) != HW::EncodeFeat(b), "a changed build state changes the bytes");
        b = a;
        b.features.push_back({ "Q", "x", "X", true });
        Check(HW::EncodeFeat(a) != HW::EncodeFeat(b), "an added feature changes the bytes");
    }

    void TestHostTiming()
    {
        std::printf("Host rules: stall grace and feat rate limit\n");
        Check(HW::StallExtensionMs(0) == 0 && HW::StallExtensionMs(1999) == 0 && HW::StallExtensionMs(2000) == 0,
            "a tick gap up to 2 s extends nothing");
        Check(HW::StallExtensionMs(2001) == 2001 && HW::StallExtensionMs(80000) == 80000, "a longer gap is added whole");

        const uint64_t grace = 30000;
        Check(!HW::LoaderGraceOver(1000 + 29999, 1000, 0, grace), "grace not over just before 30 s");
        Check(HW::LoaderGraceOver(1000 + 30000, 1000, 0, grace), "grace over at 30 s with no stall");
        Check(!HW::LoaderGraceOver(1000 + 30000, 1000, 80000, grace), "grace not over at 30 s with an 80 s stall");
        Check(!HW::LoaderGraceOver(1000 + 109999, 1000, 80000, grace), "grace not over just before 110 s");
        Check(HW::LoaderGraceOver(1000 + 110000, 1000, 80000, grace), "grace over at 110 s");
        Check(!HW::LoaderGraceOver(500, 1000, 0, grace), "a clock earlier than the manifest is not over");

        Check(HW::DecideFeatAct(false, 5000, 0) == HW::FeatGate::Act, "the first feature list acts");
        Check(HW::DecideFeatAct(true, 5999, 5000) == HW::FeatGate::Defer, "999 ms after the last act defers");
        Check(HW::DecideFeatAct(true, 6000, 5000) == HW::FeatGate::Act, "1000 ms after the last act acts");
        Check(HW::DecideFeatAct(true, 4000, 5000) == HW::FeatGate::Defer, "a clock that went back defers");
        Check(HW::DecideFeatAct(true, 5000, 5000) == HW::FeatGate::Defer, "the same millisecond defers");
    }

    void TestHostResolve()
    {
        std::printf("Host rules: resolve the deny set\n");
        HW::FeatPayload decl;
        auto add = [&](const char* plugin, const char* id) {
            HW::FeatRecord f; f.plugin = plugin; f.id = id; f.title = id; f.on = true;
            decl.features.push_back(f);
        };
        add("BetterCheats", "movement");
        add("BetterCheats", "spawner");
        add("BetterDrone", "limits");
        add("BetterDrone", "laser");
        auto has = [](const HostRules::DenySet& d, const char* p, const char* id) { return HostRules::Contains(d, p, id); };

        HW::RulesSet none;
        Check(HW::ResolveDeny(none, decl).empty(), "default allow with no rows denies nothing");

        HW::RulesSet defDeny;
        defDeny.defaultAllow = false;
        Check(HW::ResolveDeny(defDeny, decl).size() == 4, "default deny with no rows denies every declared feature");

        HW::RulesSet one;
        one.deny.push_back({ "BetterCheats", "movement" });
        HostRules::DenySet d = HW::ResolveDeny(one, decl);
        Check(d.size() == 1 && has(d, "BetterCheats", "movement"), "a feature row denies only that feature");

        HW::RulesSet whole;
        whole.deny.push_back({ "bettercheats", HW::kWholePlugin });
        d = HW::ResolveDeny(whole, decl);
        Check(d.size() == 2 && has(d, "BetterCheats", "movement") && has(d, "BetterCheats", "spawner"),
            "a whole-mod deny row (plugin name in any case) denies each declared feature, spelled as declared");

        HW::RulesSet wide;
        wide.deny.push_back({ "BetterCheats", HW::kWholePlugin });
        wide.allow.push_back({ "BetterCheats", "movement" });
        d = HW::ResolveDeny(wide, decl);
        Check(d.size() == 1 && has(d, "BetterCheats", "spawner") && !has(d, "BetterCheats", "movement"),
            "a feature allow beats a whole-mod deny");

        HW::RulesSet narrow;
        narrow.allow.push_back({ "BetterDrone", HW::kWholePlugin });
        narrow.deny.push_back({ "BetterDrone", "laser" });
        d = HW::ResolveDeny(narrow, decl);
        Check(d.size() == 1 && has(d, "BetterDrone", "laser"), "a feature deny beats a whole-mod allow");

        HW::RulesSet dd;
        dd.defaultAllow = false;
        dd.allow.push_back({ "BetterCheats", HW::kWholePlugin });
        dd.allow.push_back({ "BetterDrone", "limits" });
        d = HW::ResolveDeny(dd, decl);
        Check(d.size() == 1 && has(d, "BetterDrone", "laser"), "allow rows beat a default deny, feature and whole mod");

        HW::RulesSet both;
        both.deny.push_back({ "BetterCheats", "movement" });
        both.allow.push_back({ "BetterCheats", "movement" });
        both.deny.push_back({ "BetterDrone", HW::kWholePlugin });
        both.allow.push_back({ "BetterDrone", HW::kWholePlugin });
        d = HW::ResolveDeny(both, decl);
        Check(d.size() == 3 && has(d, "BetterCheats", "movement") && has(d, "BetterDrone", "limits") && has(d, "BetterDrone", "laser"),
            "an allow and a deny row at the same level: deny wins");

        HW::RulesSet unknown;
        unknown.deny.push_back({ "Other", HW::kWholePlugin });
        unknown.deny.push_back({ "BetterCheats", "nosuch" });
        Check(HW::ResolveDeny(unknown, decl).empty(), "rows for mods or features the client lacks name nothing");

        HW::RulesSet idCase;
        idCase.deny.push_back({ "BetterCheats", "Movement" });
        Check(HW::ResolveDeny(idCase, decl).empty(), "feature ids match exactly");

        HW::FeatPayload twice = decl;
        twice.features.push_back(twice.features[0]);
        Check(HW::ResolveDeny(defDeny, twice).size() == 4, "a feature declared twice is listed once");
        Check(HW::ResolveDeny(defDeny, HW::FeatPayload{}).empty(), "nothing declared, nothing denied");

        // Through the wire: what a server sends is what the client resolves.
        HW::RulesSet sent;
        sent.defaultAllow = false;
        sent.allow.push_back({ "BetterCheats", HW::kWholePlugin });
        sent.deny.push_back({ "BetterCheats", "spawner" });
        const std::string bytes = HW::EncodeRules(sent);
        HW::RulesSet got;
        Check(HW::DecodeRules(bytes.data(), bytes.size(), &got) == HW::Status::Ok, "the rules decode");
        d = HW::ResolveDeny(got, decl);
        Check(d.size() == 3 && has(d, "BetterCheats", "spawner") && has(d, "BetterDrone", "limits") && has(d, "BetterDrone", "laser"),
            "resolve after a wire round trip: default deny, mod allowed, one feature denied");
    }

    void TestHostMinimum()
    {
        std::printf("Host rules: minimum loader tag\n");
        using M = HW::MinCheck;
        struct Row { const char* client; const char* minimum; M want; const char* what; };
        const Row rows[] = {
            { "v1.27.0", "1.27.0", M::Ok, "equal, v prefix on one side" },
            { "1.27.0", "v1.27.0", M::Ok, "equal, v prefix on the other" },
            { "V1.27.0", "1.27.0", M::Ok, "upper case V" },
            { "1.27.1", "1.27.0", M::Ok, "patch higher" },
            { "1.28.0", "1.27.9", M::Ok, "minor higher" },
            { "2.0.0", "1.99.99", M::Ok, "major higher" },
            { "1.26.1", "1.27.0", M::Below, "minor lower" },
            { "1.27.0", "1.27.1", M::Below, "patch lower" },
            { "0.99.99", "1.0.0", M::Below, "major lower" },
            { "1.9.0", "1.10.0", M::Below, "numeric, not text: 9 under 10" },
            { "1.10.0", "1.9.0", M::Ok, "numeric, not text: 10 over 9" },
            { "1.27", "1.27.0", M::Ok, "two parts equal three with a zero" },
            { "1.27", "1.27.1", M::Below, "two parts under a patch" },
            { "1.27.0.1", "1.27.0", M::Ok, "fourth part higher" },
            { "1.27.0.0", "1.27.0.1", M::Below, "fourth part lower" },
            { "v1.26.1-dev.7", "1.26.1", M::Ok, "a dev build counts as its release part: not below its own release" },
            { "v1.26.1-dev.7", "1.26.0", M::Ok, "a dev build over an older minimum" },
            { "v1.26.1-dev.7", "1.27.0", M::Below, "a dev build under a newer minimum" },
            { "1.27.0-dev.3", "1.27.0", M::Ok, "dev of the minimum release itself is not below" },
            { "1.27.0", "1.27.0-dev.1", M::Ok, "a suffix on the minimum is ignored" },
            { "1.26.9+build5", "1.27.0", M::Below, "plus suffix" },
            { "1.27.0-rc1", "1.27.0", M::Ok, "release candidate" },
            { "dev", "1.27.0", M::Unparseable, "the unlabeled dev tag" },
            { "", "1.27.0", M::Unparseable, "empty client tag" },
            { "1.27.0", "", M::Unparseable, "empty minimum" },
            { "1.27.0", "dev", M::Unparseable, "unreadable minimum" },
            { "1", "1.27.0", M::Unparseable, "one part" },
            { "v", "1.27.0", M::Unparseable, "just a v" },
            { "1.", "1.27.0", M::Unparseable, "trailing dot" },
            { "1..2", "1.27.0", M::Unparseable, "empty part" },
            { ".1.2", "1.27.0", M::Unparseable, "leading dot" },
            { "1.2.3.4.5", "1.27.0", M::Unparseable, "five parts" },
            { "1.2abc", "1.27.0", M::Unparseable, "letters straight after the number" },
            { "1.27.0 ", "1.27.0", M::Unparseable, "trailing space" },
            { " 1.27.0", "1.27.0", M::Unparseable, "leading space" },
            { "-1.27.0", "1.27.0", M::Unparseable, "leading minus" },
            { "1.27.0-\x01", "1.27.0", M::Unparseable, "control character in the suffix" },
            { "latest", "1.27.0", M::Unparseable, "a word" },
            { "1.-2.0", "1.27.0", M::Unparseable, "negative part" },
        };
        for (const Row& r : rows)
            Check(HW::IsBelowMinimum(r.client, r.minimum) == r.want, r.what);
        Check(HW::IsBelowMinimum(std::string("1.27.0\0x", 8), "1.27.0") == M::Unparseable, "embedded NUL");
        Check(HW::IsBelowMinimum(std::string(65, '1'), "1.27.0") == M::Unparseable, "a tag over the length limit");
        Check(HW::IsBelowMinimum("99999999999.0.0", "1.27.0") == M::Ok, "a huge number is clamped, not wrapped");
    }
    // IPluginRegistry::GetPluginVersion: which plugins count as loaded, and what the caller gets back.
    struct FakePlugin
    {
        std::string name;
        std::string version;
        bool initialized = true;   // what the loader sets while the plugin runs
        bool unloading = false;    // set from the start of a shutdown or reload until it ends
    };

    bool AskVersion(const std::vector<FakePlugin>& table, const char* name, char* out, size_t cap)
    {
        return PluginVersionQuery::Find(table,
            [](const FakePlugin& p)
            {
                return PluginVersionQuery::Row{ &p.name, &p.version, p.initialized && !p.unloading };
            },
            name, out, cap);
    }

    void TestPluginVersionQuery()
    {
        std::printf("plugin version query\n");
        std::vector<FakePlugin> table;
        table.push_back({ "BetterCheats", "1.12.0-dev.3", true, false });
        table.push_back({ "Codex", "0.4.0", true, false });
        char buf[64];

        // Loaded and enabled.
        Check(AskVersion(table, "BetterCheats", buf, sizeof(buf)) && std::string(buf) == "1.12.0-dev.3", "a running plugin reports its version");
        Check(AskVersion(table, "Codex", buf, sizeof(buf)) && std::string(buf) == "0.4.0", "the second entry is found too");

        // Not present.
        std::strcpy(buf, "stale");
        Check(!AskVersion(table, "Missing", buf, sizeof(buf)) && buf[0] == '\0', "an unknown plugin is not loaded and out is empty");

        // Case rule: the plugin list's, so case does not matter.
        Check(AskVersion(table, "bettercheats", buf, sizeof(buf)) && std::string(buf) == "1.12.0-dev.3", "the name matches without regard to case");
        Check(AskVersion(table, "CODEX", buf, sizeof(buf)), "upper case matches too");
        Check(!AskVersion(table, "Better", buf, sizeof(buf)), "a part of a name does not match");

        // Present but disabled (or failed to load): the record exists, it never ran.
        table[1].initialized = false;
        Check(!AskVersion(table, "Codex", buf, sizeof(buf)), "a disabled plugin is not loaded");

        // Unloaded by the overlay RELOAD (before it loads again).
        table[0].initialized = false;
        Check(!AskVersion(table, "BetterCheats", buf, sizeof(buf)), "an unloaded plugin is not loaded");
        table[0].initialized = true;
        Check(AskVersion(table, "BetterCheats", buf, sizeof(buf)), "and it is loaded again once it runs");

        // Mid-reload: the record is out of service while its shutdown runs.
        table[0].initialized = false;
        table[0].unloading = true;
        Check(!AskVersion(table, "BetterCheats", buf, sizeof(buf)), "a plugin that is shutting down reads as not loaded");
        table[0].unloading = false;
        table[0].initialized = true;
        table[0].version = "1.12.0-dev.4";
        Check(AskVersion(table, "BetterCheats", buf, sizeof(buf)) && std::string(buf) == "1.12.0-dev.4", "after the reload it reports the new version");

        // A stopped record does not hide a running one of the same name.
        table.push_back({ "BetterCheats", "2.0.0", true, false });
        table[0].initialized = false;
        Check(AskVersion(table, "BetterCheats", buf, sizeof(buf)) && std::string(buf) == "2.0.0", "a running entry is found past a stopped one of the same name");

        // Null and empty names.
        std::strcpy(buf, "stale");
        Check(!AskVersion(table, nullptr, buf, sizeof(buf)) && buf[0] == '\0', "a null name is not loaded");
        Check(!AskVersion(table, "", buf, sizeof(buf)), "an empty name is not loaded");
        Check(!AskVersion(std::vector<FakePlugin>(), "Codex", buf, sizeof(buf)), "an empty table has no plugins");

        // Buffer size.
        table[0].initialized = true;
        table[0].version = "1.2.3";
        table.erase(table.begin() + 2);
        std::memset(buf, 'x', sizeof(buf));
        Check(AskVersion(table, "BetterCheats", buf, 6) && std::string(buf) == "1.2.3", "a buffer of exactly version plus NUL is enough");
        std::memset(buf, 'x', sizeof(buf));
        Check(!AskVersion(table, "BetterCheats", buf, 5) && buf[0] == '\0', "a buffer one byte short fails with an empty string, never a cut version");
        std::memset(buf, 'x', sizeof(buf));
        Check(!AskVersion(table, "BetterCheats", buf, 0) && buf[0] == 'x', "a size of zero fails and writes nothing");
        Check(AskVersion(table, "BetterCheats", nullptr, 0), "a null buffer only asks whether it is running");
        Check(!AskVersion(table, "Missing", nullptr, 0), "a null buffer on an unknown plugin is false");
        table[0].version.clear();
        Check(AskVersion(table, "BetterCheats", buf, sizeof(buf)) && buf[0] == '\0', "a plugin with no version text reports an empty version");
    }

    // IPluginRegistry::GetDeclaredFeatures and Logs\declared-features.v1.txt: the text and how it is handed out.
    std::string ReadAll(const std::filesystem::path& p)
    {
        std::ifstream in(p, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    void TestDeclaredFeaturesText()
    {
        using namespace DeclaredFeaturesText;
        std::printf("declared features text\n");

        const std::vector<PluginRow> plugins = {
            { "Waila", "1.2.0" }, { "Best Builder", "0.9.1" }, { "bettercheats", "1.12.0" }, { "Idle", "1.0.0" } };
        const std::vector<FeatureRow> features = {
            { "Waila", "tooltip", "Tooltip", true },
            { "BetterCheats", "Zoom", "Zoom", false },
            { "bettercheats", "alpha", "Alpha", true },
            { "Best Builder", "snap", "Snap to grid", true },
            { "Ghost", "x", "A plugin that is not running", true } };

        // Rows and order: header, then plugins by name, features by id, both without regard to case.
        const std::string text = Build(130045, "v1.26.1", plugins, features);
        const std::string want =
            "format\t1\nkind\tdeclared-features\ngame_cl\t130045\nloader\tv1.26.1\n"
            "plugin\tBest Builder\t0.9.1\n"
            "feature\tBest Builder\tsnap\ton\tSnap to grid\n"
            "plugin\tbettercheats\t1.12.0\n"
            "feature\tbettercheats\talpha\ton\tAlpha\n"
            "feature\tbettercheats\tZoom\toff\tZoom\n"
            "plugin\tIdle\t1.0.0\n"
            "plugin\tWaila\t1.2.0\n"
            "feature\tWaila\ttooltip\ton\tTooltip\n";
        Check(text == want, "header, plugins and features come out sorted without regard to case, on and off kept");
        Check(text.find("Ghost") == std::string::npos, "a feature of a plugin that is not running is left out");
        Check(text.find("plugin\tBest Builder\t0.9.1\n") != std::string::npos, "a plugin name with a space stays whole");
        Check(text.find("feature\tBest Builder\tsnap\t") != std::string::npos, "and so does its feature row");

        // The order does not depend on the input order.
        const std::vector<PluginRow> rp(plugins.rbegin(), plugins.rend());
        const std::vector<FeatureRow> rf(features.rbegin(), features.rend());
        Check(Build(130045, "v1.26.1", rp, rf) == want, "reversed input gives the same text");

        // Sanitising.
        Check(SanitiseTitle("a\tb\r\nc\x01" "d\x7f" "e") == "a b  c d e", "tab, CR, LF and control characters become spaces");
        const std::vector<FeatureRow> odd = { { "Waila", "x", "line one\nline\ttwo", true } };
        const std::string oddText = Build(1, "t", plugins, odd);
        Check(oddText.find("\tline one line two\n") != std::string::npos, "a title with a line break stays on one row");
        size_t rows = 0;
        for (char c : oddText)
            rows += (c == '\n');
        Check(rows == 4 + 4 + 1, "one row per plugin and feature, none added by a title");

        // The 80-byte cut.
        Check(SanitiseTitle(std::string(80, 'a')) == std::string(80, 'a'), "an 80-byte title is kept whole");
        Check(SanitiseTitle(std::string(81, 'a')) == std::string(80, 'a'), "an 81-byte title is cut to 80");
        const std::string e = "\xC3\xA9";  // e acute, 2 bytes
        Check(SanitiseTitle(std::string(79, 'a') + e) == std::string(79, 'a'), "a cut inside a 2-byte character drops the whole character");
        std::string forty;
        for (int i = 0; i < 40; ++i)
            forty += e;
        Check(SanitiseTitle(forty) == forty, "40 two-byte characters (80 bytes) are kept");
        Check(SanitiseTitle(forty + e) == forty, "one more character is dropped whole");
        const std::string euro = "\xE2\x82\xAC";  // 3 bytes
        Check(SanitiseTitle(std::string(78, 'a') + euro) == std::string(78, 'a'), "a cut inside a 3-byte character backs up to its start");
        Check(SanitiseTitle(std::string(77, 'a') + euro) == std::string(77, 'a') + euro, "a 3-byte character that ends at byte 80 stays");

        // Measure and cut.
        char big[1024];
        Check(CopyCut(text, nullptr, 0) == static_cast<int>(text.size()), "a null buffer with size 0 measures");
        std::memset(big, 'x', sizeof(big));
        Check(CopyCut(text, big, 0) == static_cast<int>(text.size()) && big[0] == 'x', "size 0 writes nothing");
        Check(CopyCut(text, big, sizeof(big)) == static_cast<int>(text.size()) && text == big, "a big enough buffer gets the whole text, NUL-terminated");
        char small[16];
        std::memset(small, 'x', sizeof(small));
        Check(CopyCut(text, small, sizeof(small)) == static_cast<int>(text.size()), "a small buffer still returns the full length");
        Check(small[15] == '\0' && std::strlen(small) == 15 && text.compare(0, 15, small) == 0, "and holds the start of the text, cut to size, NUL-terminated");
        char one[1] = { 'x' };
        Check(CopyCut(text, one, 1) == static_cast<int>(text.size()) && one[0] == '\0', "a size of 1 gets just the NUL");

        // The file: temp then rename.
        namespace fs = std::filesystem;
        const fs::path dir = fs::temp_directory_path() / "declared_features_test";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        const fs::path file = dir / "declared-features.v1.txt";
        Check(WriteAtomic(file, "old text\n") && ReadAll(file) == "old text\n", "a first write creates the file");
        Check(WriteAtomic(file, text) && ReadAll(file) == text, "a second write replaces the existing file");
        size_t files = 0;
        for (const fs::directory_entry& d : fs::directory_iterator(dir))
        {
            (void)d;
            ++files;
        }
        Check(files == 1 && !fs::exists(dir / "declared-features.v1.txt.tmp"), "no temp file is left behind");
        Check(!WriteAtomic(dir / "missing" / "x.txt", "t"), "a folder that is not there fails without throwing");
        fs::remove_all(dir, ec);
    }

    void TestServerAddressText()
    {
        using namespace ServerAddressText;
        std::printf("server address text\n");

        std::string out = "stale";
        Check(FromEngineText(L"203.0.113.7:7780", 17, out) && out == "203.0.113.7:7780", "an address comes through as it is");
        Check(FromEngineText(L"[2001:DB8::1]:7780", 19, out) && out == "[2001:db8::1]:7780", "upper case is lower-cased");
        Check(FromEngineText(L"HOST.Example.COM:7780", 22, out) && out == "host.example.com:7780", "a host name is lower-cased too");

        out = "stale";
        Check(!FromEngineText(L"Invalid", 8, out) && out.empty(), "the engine's \"Invalid\" is no address, and out is cleared");
        Check(!FromEngineText(L"invalid", 8, out), "\"invalid\" in any case is no address");
        Check(!FromEngineText(L"INVALID", 8, out), "\"INVALID\" is no address either");
        Check(FromEngineText(L"invalid.example:7780", 21, out) && out == "invalid.example:7780", "a text that only starts with \"invalid\" is an address");

        out = "stale";
        Check(!FromEngineText(L"", 1, out) && out.empty(), "an empty FString (num 1, just the NUL) is no address");
        Check(!FromEngineText(L"", 0, out), "num 0 is no address");
        Check(!FromEngineText(L"1.2.3.4:5", -3, out), "a negative num is no address");
        Check(!FromEngineText(nullptr, 17, out), "null data is no address");
        Check(!FromEngineText(nullptr, 0, out), "null data with num 0 is no address");
        Check(!FromEngineText(L"\0" L"1.2.3.4:5", 11, out), "text that starts with a NUL is empty, so no address");

        const std::wstring longText(kMaxUnits, L'a');
        Check(FromEngineText(longText.c_str(), kMaxUnits + 1, out) && out.size() == static_cast<size_t>(kMaxUnits), "the longest accepted text is taken whole");
        const std::wstring tooLong(kMaxUnits + 1, L'a');
        Check(!FromEngineText(tooLong.c_str(), kMaxUnits + 2, out) && out.empty(), "one unit over the cap is no address");
        Check(!FromEngineText(L"1.2.3.4:5", 0x7FFFFFFF, out), "a huge num is rejected before anything is read");

        Check(FromEngineText(L"1.2.3.4\0" L"junk", 13, out) && out == "1.2.3.4", "an embedded NUL ends the text");

        // UTF-16 to UTF-8.
        Check(WideToUtf8(L"\xE9", 1) == "\xC3\xA9", "a two-byte character");
        Check(WideToUtf8(L"\x20AC", 1) == "\xE2\x82\xAC", "a three-byte character");
        Check(WideToUtf8(L"\xD83D\xDE00", 2) == "\xF0\x9F\x98\x80", "a surrogate pair becomes one four-byte character");
        Check(WideToUtf8(L"\xD83D", 1) == "\xEF\xBF\xBD", "a high surrogate at the end becomes U+FFFD");
        Check(WideToUtf8(L"\xD83D" L"a", 2) == "\xEF\xBF\xBD" "a", "a high surrogate without a partner becomes U+FFFD and the next unit stays");
        Check(WideToUtf8(L"\xDE00", 1) == "\xEF\xBF\xBD", "a lone low surrogate becomes U+FFFD");
        Check(FromEngineText(L"H\xC9" L"st:1", 7, out) && out == "h\xC3\x89st:1", "non-ASCII text comes through as UTF-8, only ASCII letters are lower-cased");

        // The copy into the caller's buffer.
        const std::string text = "203.0.113.7:7780";
        using HostRulesWire::CopyTextCut;
        Check(CopyTextCut(text, nullptr, 0) == static_cast<int>(text.size()), "a null buffer with size 0 measures");
        char big[64];
        std::memset(big, 'x', sizeof(big));
        Check(CopyTextCut(text, big, 0) == static_cast<int>(text.size()) && big[0] == 'x', "size 0 writes nothing");
        char one[1] = { 'x' };
        Check(CopyTextCut(text, one, 1) == static_cast<int>(text.size()) && one[0] == '\0', "size 1 gets just the NUL");
        char exact[17];
        std::memset(exact, 'x', sizeof(exact));
        Check(CopyTextCut(text, exact, sizeof(exact)) == static_cast<int>(text.size()) && text == exact, "a buffer one over the length gets the whole text");
        char tight[16];
        std::memset(tight, 'x', sizeof(tight));
        Check(CopyTextCut(text, tight, sizeof(tight)) == static_cast<int>(text.size()) && tight[15] == '\0' && text.compare(0, 15, tight) == 0, "a buffer of exactly the length is cut by one and NUL-terminated");
        Check(CopyTextCut(text, big, sizeof(big)) == static_cast<int>(text.size()) && text == big, "a big buffer gets the whole text");
    }

    // plugin-catalog.v1.txt: grammar and cache rules.
    void TestPluginCatalogText()
    {
        using namespace PluginCatalogText;
        std::printf("plugin catalog text\n");

        const std::string head = "format\t1\nseq\t42\nkind\tplugin-catalog\n";
        const std::string good = head + "plugin\tCodex\t1.2.0\nplugin\tBest Builder\t0.9.1-rc.1+b5\n"
                                        "feature\tCodex\tpins\t-\tPinned recipes\n"
                                        "feature\tBest Builder\tgrid\t*\tSnap to grid\n"
                                        "feature\tCodex\tmap_view\t127004,128786\tMap view\n";
        auto Ok = [](const std::string& body) { Catalog c; std::string e; return Parse(body, c, e); };
        auto OneFeature = [&](const std::string& tail) { return head + "plugin\tCodex\t1\n" + tail; };
        auto OneRow = [&](const std::string& row) { return head + row + "\n"; };

        Catalog c;
        std::string err;
        Check(Parse(good, c, err) && err.empty(), "a good file parses");
        Check(c.seq == 42 && c.plugins.size() == 2 && c.features.size() == 3, "seq, plugins and features are read");
        Check(c.plugins[1].name == "Best Builder" && c.plugins[1].version == "0.9.1-rc.1+b5", "name and version are kept");
        Check(c.features[2].off == "127004,128786" && c.features[0].title == "Pinned recipes", "off and title are kept");
        Check(ToAccessorText(c) == good, "round trip is byte-stable for a canonical file");
        Check(!Parse("", c, err) && c.plugins.empty() && c.seq == 0 && !err.empty(), "a refused file leaves out empty and sets error");

        // Names.
        auto Name = [&](const std::string& n) { return Ok(OneRow("plugin\t" + n + "\t1")); };
        Check(Name("Best Builder") && Name("A_b-9") && Name("a b c"), "names: letters, digits, _ and -, single inner spaces");
        Check(!Name("Best  Builder") && !Name(" Best") && !Name("Best ") && !Name("") && !Name("Best.Builder") && !Name("Bést"),
            "names: double, leading, trailing space, empty and other characters refused");
        Check(Name(std::string(64, 'a')) && !Name(std::string(65, 'a')), "names: 64 bytes ok, 65 refused");

        // Ids.
        auto Id = [&](const std::string& id) { return Ok(OneFeature("feature\tCodex\t" + id + "\t-\tT\n")); };
        Check(Id("a_B-9") && Id(std::string(64, 'x')) && !Id(std::string(65, 'x')), "ids: 64 bytes ok, 65 refused");
        Check(!Id("") && !Id("a b") && !Id("a.b"), "ids: empty, space and dot refused");

        // Versions.
        auto Ver = [&](const std::string& v) { return Ok(OneRow("plugin\tCodex\t" + v)); };
        Check(Ver("1.2.3-rc_1+x") && Ver(std::string(32, '1')) && !Ver(std::string(33, '1')), "versions: 32 bytes ok, 33 refused");
        Check(!Ver("") && !Ver("1 2") && !Ver("v1:2"), "versions: empty, space and colon refused");

        // Titles.
        auto Title = [&](const std::string& t) { return Ok(OneFeature("feature\tCodex\tid\t-\t" + t + "\n")); };
        Check(Title(std::string(80, 'a')) && !Title(std::string(81, 'a')) && !Title(""), "titles: 80 bytes ok, 81 and empty refused");
        const std::string eAcute = "\xC3\xA9";   // 2 bytes
        Check(Title(std::string(78, 'a') + eAcute) && !Title(std::string(79, 'a') + eAcute),
            "titles: a multi-byte character ending exactly at byte 80 is ok, one ending at 81 is refused");
        Check(Title("\xE2\x82\xAC \xF0\x9F\x98\x80"), "titles: 3 and 4 byte characters ok");
        Check(!Title("a\xC3") && !Title("\xC3(") && !Title("\xC0\x80") && !Title("\xED\xA0\x80") && !Title("\xF4\x90\x80\x80") && !Title("\xFF"),
            "titles: truncated, bad continuation, overlong, surrogate, over U+10FFFF and 0xFF refused");
        Check(!Title("a\x01" "b") && !Title("a\x7F" "b") && !Title("a\xC2\x85" "b"), "titles: C0, DEL and C1 control characters refused");
        Check(!Ok(OneFeature("feature\tCodex\tid\t-\ta\tb\n")), "titles: a tab makes a sixth field and is refused");

        // Off.
        auto Off = [&](const std::string& o) { return Ok(OneFeature("feature\tCodex\tid\t" + o + "\tT\n")); };
        Check(Off("-") && Off("*") && Off("127004") && Off("127004,128786") && Off("0,1,4294967295"), "off: -, *, one CL and an ascending list ok");
        Check(!Off("128786,127004") && !Off("127004,127004") && !Off("") && !Off("127004,") && !Off(",127004"),
            "off: unsorted, duplicated, empty and dangling comma refused");
        Check(!Off("127004, 128786") && !Off("12x") && !Off("-1") && !Off("*,1") && !Off("007") && !Off("12345678901"),
            "off: space, non-digit, sign, mixed, leading zero and over 10 digits refused");

        // Rows and references.
        Check(!Ok(OneFeature("feature\tNobody\tid\t-\tT\n")), "a feature for an unknown plugin is refused");
        Check(!Ok(OneFeature("feature\tcodex\tid\t-\tT\n")), "a feature's plugin must match exactly, case included");
        Check(!Ok(head + "plugin\tCodex\t1\nplugin\tCODEX\t1\n"), "plugins differing only in case are duplicates");
        Check(Ok(OneFeature("feature\tCodex\tID\t-\tT\nfeature\tCodex\tid\t-\tT\n")), "feature ids differing in case are distinct");
        Check(!Ok(OneFeature("feature\tCodex\tid\t-\tT\nfeature\tCodex\tid\t*\tU\n")), "a duplicate feature id is refused");
        Check(Ok(head + "plugin\tA\t1\nplugin\tB\t1\nfeature\tA\tid\t-\tT\nfeature\tB\tid\t-\tT\n"), "the same id under two plugins is fine");
        Check(!Ok(head + "plugin\tA\t1\nfeature\tA\tid\t-\tT\nplugin\tB\t1\n"), "a plugin row after a feature row is refused");
        Check(!Ok(head), "a file with no plugin row is refused");
        Check(!Ok(OneRow("feature\tCodex\tid\t-\tT")), "a feature before any plugin is refused");
        Check(!Ok(OneRow("plugin\tCodex")) && !Ok(OneRow("plugin\tCodex\t1\textra")), "plugin rows with too few or too many fields refused");
        Check(!Ok(OneFeature("feature\tCodex\tid\t-\n")) && !Ok(OneFeature("feature\tCodex\tid\t-\tT\tx\n")), "feature rows with wrong field counts refused");
        Check(!Ok(OneFeature("bogus\tx\n")), "an unknown row kind is refused");

        // Header.
        Check(!Ok("seq\t42\nformat\t1\nkind\tplugin-catalog\nplugin\tCodex\t1\n"), "header rows out of order are refused");
        Check(!Ok("format\t1\nkind\tplugin-catalog\nplugin\tCodex\t1\n"), "a missing seq is refused");
        Check(!Ok("format\t2\nseq\t1\nkind\tplugin-catalog\nplugin\tCodex\t1\n"), "format other than 1 is refused");
        Check(!Ok("format\t1\nseq\t1\nkind\tdeclared-features\nplugin\tCodex\t1\n"), "another kind is refused");
        auto Seq = [&](const std::string& s) { return Ok("format\t1\nseq\t" + s + "\nkind\tplugin-catalog\nplugin\tCodex\t1\n"); };
        Check(Seq("1") && Seq("18446744073709551615"), "seq: decimal up to 2^64-1 ok");
        Check(!Seq("18446744073709551616") && !Seq("") && !Seq("0x10") && !Seq("-1") && !Seq("1.5") && !Seq("01") && !Seq("1 "),
            "seq: overflow, empty, hex, sign, fraction, leading zero and space refused");

        // File shape.
        Check(!Ok(good.substr(0, good.size() - 1)), "no trailing LF is refused");
        Check(!Ok(good + "\n"), "two trailing LFs are refused");
        Check(!Ok(head + "\nplugin\tCodex\t1\n"), "an empty row is refused");
        std::string crlf;
        for (char ch : good) { if (ch == '\n') crlf += '\r'; crlf += ch; }
        Check(!Ok(crlf), "CR in the file is refused");

        // Limits.
        auto Plugins = [&](size_t n) {
            std::string s = head;
            for (size_t i = 0; i < n; ++i) s += "plugin\tP" + std::to_string(i) + "\t1\n";
            return s;
        };
        Check(Ok(Plugins(200)) && !Ok(Plugins(201)), "200 plugins ok, 201 refused");
        auto Features = [&](size_t n) {
            std::string s = head + "plugin\tCodex\t1\n";
            for (size_t i = 0; i < n; ++i) s += "feature\tCodex\tf" + std::to_string(i) + "\t-\tT\n";
            return s;
        };
        Check(Ok(Features(2000)) && !Ok(Features(2001)), "2000 features ok, 2001 refused");

        // A valid file of exactly 256 KiB: feature rows with a 64-byte id, their titles sized to fill it.
        const size_t target = 256 * 1024;
        std::string full = head + "plugin\tP\t1\n";
        const size_t fixedRow = std::string("feature\tP\t").size() + 64 + std::string("\t-\t\n").size();
        const size_t room = target - full.size();
        const size_t rows = (room + fixedRow + 79) / (fixedRow + 80);
        const size_t titleBytes = room - rows * fixedRow;
        for (size_t i = 0; i < rows; ++i)
        {
            std::string id = "i" + std::to_string(i);
            id.resize(64, 'x');
            full += "feature\tP\t" + id + "\t-\t" + std::string(titleBytes / rows + (i < titleBytes % rows ? 1 : 0), 'T') + "\n";
        }
        Check(full.size() == target && Ok(full), "a file of exactly 256 KiB is accepted");
        Check(!Ok(full + "x") && !Ok(full.substr(0, full.size() - 1) + "x\n"), "one byte over 256 KiB is refused");

        // Cache rules.
        Cached cache{ 42, good };
        const std::string other = good + "plugin\tMore\t1\n";
        Check(DecideCache(nullptr, 1, good) == CacheDecision::Replace, "no cache: the candidate is taken");
        Check(DecideCache(&cache, 43, other) == CacheDecision::Replace, "a higher seq replaces the cache");
        Check(DecideCache(&cache, 41, other) == CacheDecision::RefuseRollback, "a lower seq is refused as a rollback");
        Check(DecideCache(&cache, 42, good) == CacheDecision::Unchanged, "equal seq and identical bytes is unchanged");
        Check(DecideCache(&cache, 42, other) == CacheDecision::RefuseConflict, "equal seq and different bytes is refused");
    }
}

int main()
{
    TestEvaluate();
    TestAcceptList();
    TestCodec();
    TestAckTail();
    TestClassify();
    TestWarnOnce();
    TestMirror();
    TestHostFeat();
    TestHostGameBuild();
    TestHostRules();
    TestHostPlan();
    TestHostMinimum();
    TestHostResolve();
    TestHostFeatStable();
    TestHostTiming();
    TestPluginVersionQuery();
    TestDeclaredFeaturesText();
    TestServerAddressText();
    TestPluginCatalogText();

    if (g_failures)
    {
        std::printf("\n%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("\nall checks passed\n");
    return 0;
}
