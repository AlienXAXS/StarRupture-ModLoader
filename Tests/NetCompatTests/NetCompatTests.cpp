// NetCompatTests: the plugin packet compatibility rule and the wire table, with no game.
//
// Compiles the loader's own network_channel\wire_compat.h, which has no game or
// Windows dependency. See Tests/README.md.

#include "wire_compat.h"

#include <cstdio>
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
}

int main()
{
    TestEvaluate();
    TestAcceptList();
    TestCodec();
    TestAckTail();
    TestClassify();
    TestWarnOnce();

    if (g_failures)
    {
        std::printf("\n%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("\nall checks passed\n");
    return 0;
}
