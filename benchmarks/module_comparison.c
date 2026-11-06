#include "mcmux.h"

#ifdef MCMUX_BENCHMARK_C
#include "module-baseline-meta.h"
static bool baseline_run;
extern RsResult baseline_sim_use_c(World *, BlockPos, BlockPos, const SimCModel *);
extern RsResult baseline_sim_add_c(World *, BlockPos, BlockPos, const SimCModel *);
extern RsResult baseline_sim_add_cpu_instance(World *, BlockPos, BlockPos);
extern bool baseline_sim_cpu_active(const World *);
extern bool baseline_sim_c_active(const World *);
static RsResult comparison_use_c(World *w, BlockPos a, BlockPos b, const SimCModel *m)
{ return baseline_run ? baseline_sim_use_c(w,a,b,m) : sim_use_c(w,a,b,m); }
static RsResult comparison_add_c(World *w, BlockPos a, BlockPos b, const SimCModel *m)
{ return baseline_run ? baseline_sim_add_c(w,a,b,m) : sim_add_c(w,a,b,m); }
static RsResult comparison_add_instance(World *w, BlockPos a, BlockPos b)
{ return baseline_run ? baseline_sim_add_cpu_instance(w,a,b) : sim_add_cpu_instance(w,a,b); }
static bool comparison_cpu_active(const World *w)
{ return baseline_run ? baseline_sim_cpu_active(w) : sim_cpu_active(w); }
static bool comparison_c_active(const World *w)
{ return baseline_run ? baseline_sim_c_active(w) : sim_c_active(w); }
#define sim_use_c comparison_use_c
#define sim_add_c comparison_add_c
#define sim_add_cpu_instance comparison_add_instance
#define sim_cpu_active comparison_cpu_active
#define sim_c_active comparison_c_active
#endif

/* Reuse the maintained fixture, inputs, trace verification and timing window.
 * Only backend attachment/inspection is selected above. Native reference
 * verification stays reference; both timed runs attach compiled C backends. */
#define main scaling_main_unused
#include "redstone_scaling.c"
#undef main
#ifdef MCMUX_BENCHMARK_C
#undef sim_use_c
#undef sim_add_c
#undef sim_add_cpu_instance
#undef sim_cpu_active
#undef sim_c_active
#endif

enum { NORMAL_CASES = sizeof(scenarios)/sizeof(*scenarios), MODULE_CASES = NORMAL_CASES + 1 };
static CsaLayout comparison_layout(size_t index)
{ return index == NORMAL_CASES ? CSA_SHARED : CSA_WHOLE; }

static void comparison_help(void)
{
    puts("Usage: ./nob benchmark --compare-modules [--baseline REF] [--quick] [--rounds N] [--verbose]\n"
         "Compare baseline and current C executors in alternating order.\n"
         "--baseline selects a local Git commit/ref (default: HEAD, before committing a patch).\n"
         "Includes the six unchanged default workloads and 4096 shared CSA modules.\n"
         "Both versions are checked against native reference execution.\n"
         "One report is saved automatically; the working tree is never switched.\n"
         "--quick shortens the six ordinary cases; the 4096-module case stays 128 gt.\n"
         "Also accepts --seed N, --output PATH, --no-output and --no-verify.");
}

static bool emit_comparison_models(const char *path, Scenario *cases)
{
    FILE *out = fopen(path, "w");
    if (!out) return false;
    bool ok = true;
    double start = runtime_clock_now();
    for (size_t i = 0; i < MODULE_CASES && ok; ++i) {
        Fixture fixture = {0};
        Config config = cases[i].config;
        csa_layout = comparison_layout(i);
        if (config.cell == CELL_CSA && csa_layout == CSA_SHARED) config.width = 1;
        ok = fixture_build(&config, &fixture);
        if (ok) {
            char symbol[64];
            snprintf(symbol, sizeof(symbol), "mcmux_bench_%zu", i);
            ok = sim_write_c(fixture.world, fixture.min, fixture.max, out, symbol) == RS_OK;
        }
        fixture_destroy(&fixture);
    }
    fputs("\nstatic const SimCModel *const mcmux_benchmark_models[] = {\n", out);
    for (size_t i = 0; i < MODULE_CASES; ++i) fprintf(out, " &mcmux_bench_%zu,\n", i);
    fprintf(out, "};\nstatic const double mcmux_c_generation_seconds = %.9f;\n",
            runtime_clock_now() - start);
    if (ferror(out)) ok = false;
    if (fclose(out)) ok = false;
    if (!ok) (void)remove(path);
    return ok;
}

#ifdef MCMUX_BENCHMARK_C
static bool comparison_case(const Scenario *scenario, size_t index,
                            BenchmarkResult *result, double *baseline_prepare)
{
    const Config *config = &scenario->config;
    selected_c_model = mcmux_benchmark_models[index];
    csa_layout = comparison_layout(index);
    *result = (BenchmarkResult){.scenario = *scenario};
    if (config->verify) {
        VerifyResult before, after;
        fprintf(stderr, "[%zu/%u] %s verifying both versions\n", index+1, MODULE_CASES, scenario->name);
        baseline_run = true;
        if (!verify_equivalence(config, &before)) return false;
        baseline_run = false;
        if (!verify_equivalence(config, &after)) return false;
        if (before.writes != after.writes || before.digest != after.digest) return false;
        result->verification = after;
    }
    for (uint32_t round = 0; round < config->rounds; ++round) {
        RunResult runs[2];
        for (unsigned turn = 0; turn < 2; ++turn) {
            unsigned version = turn ^ (round & 1u);
            baseline_run = version == 0;
            if (!timed_run(config, true, &runs[version])) return false;
        }
        if (runs[0].digest != runs[1].digest || !work_stats_equal(runs[0].stats,runs[1].stats)) {
            fprintf(stderr, "comparison state/work mismatch: %s pair %u\n", scenario->name, round+1);
            return false;
        }
        result->reference_rounds[round] = runs[0].seconds;
        result->cpu_rounds[round] = runs[1].seconds;
        result->prepare_rounds[round] = runs[1].prepare_seconds;
        baseline_prepare[round] = runs[0].prepare_seconds;
        result->speedup_rounds[round] = runs[0].seconds/runs[1].seconds;
        result->cpu_stats = runs[1].stats;
        fprintf(stderr, "[%zu/%u] %s pair %u/%u\n", index+1,MODULE_CASES,
                scenario->name,round+1,config->rounds);
    }
    result->reference_seconds = median_copy(result->reference_rounds,config->rounds);
    result->cpu_seconds = median_copy(result->cpu_rounds,config->rounds);
    result->prepare_seconds = median_copy(result->prepare_rounds,config->rounds);
    result->paired_speedup = median_copy(result->speedup_rounds,config->rounds);
    return true;
}

static void comparison_report(Report *report, const Options *options,
                               const BenchmarkResult *results, double setup[][31])
{
    report_printf(report,"# mcmux module executor comparison\n\n");
    report_printf(report,"Baseline executor: `%s`. Current: working tree (source fingerprint `%s`).\n\n",
                  MCMUX_BASELINE_EXECUTOR,MCMUX_CURRENT_EXECUTOR);
    report_printf(report,"Both executors use the same current common runtime, generated queries and inputs. "
                  "Timed pairs alternate baseline/current order. Native reference checks are untimed.\n\n");
    report_printf(report,"Platform: `%s`. Compiler: `%s`. Correctness: **%s**.\n\n",
                  platform_name(),
#ifdef __VERSION__
                  __VERSION__,
#else
                  "unknown",
#endif
                  options->verify?"PASS (both versions)":"SKIPPED");
    report_printf(report,"AOT generation: %.6f s. Compile/link (both executors): %.6f s. "
                  "Preparation is reported separately from execution.\n\n",
                  mcmux_c_generation_seconds,options->c_build_seconds);
    report_printf(report,"| Scenario | Layout | Modules | Game ticks | Baseline C | Current C | Time change | Baseline setup | Current setup |\n"
                  "|:--|:--|--:|--:|--:|--:|:--|--:|--:|\n");
    for(size_t i=0;i<MODULE_CASES;++i) {
        const BenchmarkResult *r=&results[i];const Config *c=&r->scenario.config;
        char before[32],after[32],change[48];
        format_duration(before,sizeof(before),r->reference_seconds);
        format_duration(after,sizeof(after),r->cpu_seconds);
        format_time_change(change,sizeof(change),r->reference_seconds,r->cpu_seconds);
        report_printf(report,"| `%s` | `%s` | %llu | %u | %s | %s | **%s** | %.3f ms | %.3f ms |\n",
            r->scenario.name,layout_name(comparison_layout(i)),(unsigned long long)((uint64_t)c->width*c->depth),
            c->ticks,before,after,change,median_copy(setup[i],c->rounds)*1000,r->prepare_seconds*1000);
    }
    report_printf(report,"\n## Verification and work\n\n"
        "| Scenario | Ordered writes | Digest | Computed | Cache hits | Fallbacks | Neighbor checks | Wires visited | Wire changes | Events | Pending |\n"
        "|:--|--:|:--|--:|--:|--:|--:|--:|--:|--:|--:|\n");
    for(size_t i=0;i<MODULE_CASES;++i) {
        const BenchmarkResult *r=&results[i];SimStats s=r->cpu_stats;
        report_printf(report,"| `%s` | %llu | `%016llx` | %llu | %llu | %llu | %llu | %llu | %llu | %llu | %zu |\n",
            r->scenario.name,(unsigned long long)r->verification.writes,(unsigned long long)r->verification.digest,
            (unsigned long long)s.cpu_evaluations,(unsigned long long)s.cpu_cache_hits,(unsigned long long)s.cpu_fallbacks,
            (unsigned long long)s.neighbor_checks,(unsigned long long)s.wires_visited,(unsigned long long)s.wire_changes,
            (unsigned long long)s.events_processed,s.pending_events);
    }
    report_printf(report,"\n## Timed pairs\n\n");
    for(size_t i=0;i<MODULE_CASES;++i) {
        const BenchmarkResult *r=&results[i];
        report_printf(report,"### `%s`\n\n| Pair | Baseline C | Current C | Baseline setup | Current setup |\n|--:|--:|--:|--:|--:|\n",r->scenario.name);
        for(uint32_t j=0;j<r->scenario.config.rounds;++j)
            report_printf(report,"| %u | %.3f ms | %.3f ms | %.3f ms | %.3f ms |\n",j+1,
                r->reference_rounds[j]*1000,r->cpu_rounds[j]*1000,setup[i][j]*1000,r->prepare_rounds[j]*1000);
        report_printf(report,"\n");
    }
    report_printf(report,"## Machine-readable results\n\n```text\n");
    for(size_t i=0;i<MODULE_CASES;++i) {
        const BenchmarkResult *r=&results[i];const Config *c=&r->scenario.config;
        report_printf(report,"COMPARE_RESULT scenario=%s layout=%s modules=%llu ticks=%u period=%u pattern=%s seed=%llu rounds=%u verify=%s baseline_seconds=%.9f current_seconds=%.9f speedup=%.6f baseline_prepare_seconds=%.9f current_prepare_seconds=%.9f\n",
            r->scenario.name,layout_name(comparison_layout(i)),(unsigned long long)((uint64_t)c->width*c->depth),c->ticks,c->period,
            pattern_name(c->pattern),(unsigned long long)c->seed,c->rounds,options->verify?"yes":"no",
            r->reference_seconds,r->cpu_seconds,r->paired_speedup,median_copy(setup[i],c->rounds),r->prepare_seconds);
    }
    report_printf(report,"COMPARISON_SUITE baseline_executor=%s current_source=%s scenarios=%u verify=%s\n```\n",
                  MCMUX_BASELINE_EXECUTOR,MCMUX_CURRENT_EXECUTOR,MODULE_CASES,options->verify?"yes":"no");
}
#endif

int main(int argc, char **argv)
{
#ifdef MCMUX_BENCHMARK_C
    bool explicit_output=false;
#endif
    for(int i=1;i<argc;++i) {
        if(!strcmp(argv[i],"--help") || !strcmp(argv[i],"-h")) {comparison_help();return 0;}
        if(!strcmp(argv[i],"--list-scenarios")) {list_scenarios();puts("  shared-csa-4096 (shared, random,4096 modules,128 gt)");return 0;}
#ifdef MCMUX_BENCHMARK_C
        if(!strcmp(argv[i],"--output")) explicit_output=true;
#endif
    }
    Options options;
    if(!parse_options(argc,argv,&options)) return 2;
    if(options.mode!=RUN_SUITE || options.csa_layout!=CSA_WHOLE) {
        fputs("module comparison fixes workloads/layouts; use ordinary benchmark for custom cases\n",stderr);return 2;
    }
    Scenario cases[MODULE_CASES];memcpy(cases,scenarios,sizeof(scenarios));
    for(size_t i=0;i<NORMAL_CASES;++i) apply_options(&cases[i],&options);
    cases[NORMAL_CASES]=(Scenario){"shared-csa-4096","Large shared CSA bank",
        {CELL_CSA,PATTERN_RANDOM,4096,1,128,8,options.rounds_override?options.rounds_override:options.quick?3:7,options.seed,options.verify}};
    for(size_t i=0;i<MODULE_CASES;++i) if(!validate_config(&cases[i].config)) return 2;
    if(options.emit_c_path) return emit_comparison_models(options.emit_c_path,cases)?0:1;
#ifdef MCMUX_BENCHMARK_C
    BenchmarkResult results[MODULE_CASES];double setup[MODULE_CASES][31]={{0}};
    for(size_t i=0;i<MODULE_CASES;++i) if(!comparison_case(&cases[i],i,&results[i],setup[i])) return 1;
    char path[256];const char *output=options.output_path;FILE *copy=NULL;
    if(options.write_report) {
        if(explicit_output) copy=fopen(output,"w");
        else for(unsigned i=1;i<=9999;++i) {
            snprintf(path,sizeof(path),"build/benchmark-module-comparison-%03u.md",i);
            copy=fopen(path,"wx");
            if(copy) {output=path;break;}
            if(errno!=EEXIST) break;
        }
        if(!copy) {fputs("cannot create comparison report\n",stderr);return 1;}
    }
    Report report={.copy=copy};comparison_report(&report,&options,results,setup);fflush(stdout);
    if(copy && fclose(copy)) return 1;
    if(options.write_report) fprintf(stderr,"Comparison report written to %s\n",output);
    return 0;
#else
    fputs("run ./nob benchmark --compare-modules\n",stderr);return 2;
#endif
}
