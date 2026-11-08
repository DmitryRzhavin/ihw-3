#!/usr/bin/env bash
set -euo pipefail
[[ $# -gt 0 ]] || { echo "Usage: bash scripts/check_log.sh LOG..." >&2; exit 2; }
for log in "$@"; do
    awk '
    function bad(message) {
        print FILENAME ":" FNR ": " message > "/dev/stderr"
        failed=1
        exit 1
    }
    function field(key) { return values[key]+0 }
    {
        line=$0
        sub(/^.*\] /,"",line)
        count=split(line,words,/[[:space:]]+/)
        kind=words[1]
        for (key in values) delete values[key]
        for(i=2;i<=count;i++) {
            n=split(words[i],pair,"=")
            if(n==2) values[pair[1]]=pair[2]
        }
        o=field("order"); d=field("dish"); c=field("cook"); step=field("step")
        key=o SUBSEP d
        if(kind=="INVARIANT_FAILURE") bad("model invariant failed")
        if(kind=="ARRIVAL") {
            visitor=field("visitor")
            if(visitor in arrived) bad("duplicate arrival")
            arrived[visitor]=1
        } else if(kind=="ACCEPT") {
            if(field("visitor")!=o || !(o in arrived) || (o in accepted)) bad("invalid acceptance")
            accepted[o]=1
        } else if(kind=="CREATE") {
            if(!(o in accepted)) bad("operation created before acceptance")
            if(step>recipe[key]) recipe[key]=step
        } else if(kind=="EQUIPMENT") {
            unit=field("type") SUBSEP field("unit")
            if((unit in occupied) || (c in acquired)) bad("equipment double use")
            occupied[unit]=c; acquired[c]=unit
        } else if(kind=="START") {
            if((c in active) || !(c in acquired)) bad("cook double use or missing equipment")
            if(step!=last[key]+1) bad("operation dependency violated")
            if(step>recipe[key]) bad("unknown operation")
            active[c]=key SUBSEP step SUBSEP field("job")
        } else if(kind=="END") {
            expected=key SUBSEP step SUBSEP field("job")
            if(!(c in active) || active[c]!=expected) bad("finish does not match start")
            delete active[c]; delete occupied[acquired[c]]; delete acquired[c]
            last[key]=step; ends++
        } else if(kind=="DISH_READY") {
            if(!(key in last) || last[key]!=recipe[key] || (key in cooked)) bad("dish ready too early or twice")
            cooked[key]=1; ready_count[o]++
        } else if(kind=="COLLECT") {
            if(!(key in cooked) || (key in collected)) bad("invalid dish collection")
            collected[key]=1; collected_count[o]++
        } else if(kind=="ASSEMBLED") {
            if((o in assembled) || ready_count[o]!=field("dishes") || collected_count[o]!=field("dishes")) bad("invalid assembly")
            assembled[o]=1
        } else if(kind=="DELIVER") {
            if(o!=field("visitor") || !(o in assembled) || (o in delivered) || (o in canceled)) bad("invalid delivery")
            delivered[o]=1; delivered_count++
        } else if(kind=="CANCELED") {
            if(!(o in accepted) || (o in delivered) || (o in canceled)) bad("invalid cancellation")
            canceled[o]=1; canceled_count++
        } else if(kind=="SUMMARY") {
            summaries++
            normal=(values["termination"]=="normal")
            total=field("total"); summary_delivered=field("delivered")
            summary_canceled=field("canceled"); started=field("started"); completed=field("completed")
        }
    }
    END {
        if(failed) exit 1
        if(summaries!=1) bad("expected one summary")
        if(normal) {
            for(c in active) bad("unfinished cook operation")
            for(unit in occupied) bad("occupied equipment at termination")
            for(c in acquired) bad("equipment not released")
            if(delivered_count+canceled_count!=total) bad("orders missing at termination")
            if(summary_delivered!=delivered_count || summary_canceled!=canceled_count) bad("summary mismatch")
            if(started!=completed || ends!=completed) bad("operation counters mismatch")
        }
    }' "$log"
done
