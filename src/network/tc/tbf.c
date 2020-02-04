/* SPDX-License-Identifier: LGPL-2.1+
 * Copyright © 2019 VMware, Inc. */

#include <linux/pkt_sched.h>
#include <math.h>

#include "alloc-util.h"
#include "conf-parser.h"
#include "netem.h"
#include "netlink-util.h"
#include "networkd-manager.h"
#include "parse-util.h"
#include "qdisc.h"
#include "string-util.h"
#include "tc-util.h"
#include "util.h"

static int token_buffer_filter_fill_message(Link *link, QDisc *qdisc, sd_netlink_message *req) {
        uint32_t rtab[256], ptab[256];
        struct tc_tbf_qopt opt = {};
        TokenBufferFilter *tbf;
        int r;

        assert(link);
        assert(qdisc);
        assert(req);

        tbf = TBF(qdisc);

        opt.rate.rate = tbf->rate >= (1ULL << 32) ? ~0U : tbf->rate;
        opt.peakrate.rate = tbf->peak_rate >= (1ULL << 32) ? ~0U : tbf->peak_rate;

        if (tbf->limit > 0)
                opt.limit = tbf->limit;
        else {
                double lim, lim2;

                lim = tbf->rate * (double) tbf->latency / USEC_PER_SEC + tbf->burst;
                if (tbf->peak_rate > 0) {
                        lim2 = tbf->peak_rate * (double) tbf->latency / USEC_PER_SEC + tbf->mtu;
                        lim = MIN(lim, lim2);
                }
                opt.limit = lim;
        }

        opt.rate.mpu = tbf->mpu;

        r = tc_fill_ratespec_and_table(&opt.rate, rtab, tbf->mtu);
        if (r < 0)
                return log_link_error_errno(link, r, "Failed to calculate ratespec: %m");

        r = tc_transmit_time(opt.rate.rate, tbf->burst, &opt.buffer);
        if (r < 0)
                return log_link_error_errno(link, r, "Failed to calculate buffer size: %m");

        if (opt.peakrate.rate > 0) {
                opt.peakrate.mpu = tbf->mpu;

                r = tc_fill_ratespec_and_table(&opt.peakrate, ptab, tbf->mtu);
                if (r < 0)
                        return log_link_error_errno(link, r, "Failed to calculate ratespec: %m");

                r = tc_transmit_time(opt.peakrate.rate, tbf->mtu, &opt.mtu);
                if (r < 0)
                        return log_link_error_errno(link, r, "Failed to calculate mtu size: %m");
        }

        r = sd_netlink_message_open_container_union(req, TCA_OPTIONS, "tbf");
        if (r < 0)
                return log_link_error_errno(link, r, "Could not open container TCA_OPTIONS: %m");

        r = sd_netlink_message_append_data(req, TCA_TBF_PARMS, &opt, sizeof(struct tc_tbf_qopt));
        if (r < 0)
                return log_link_error_errno(link, r, "Could not append TCA_TBF_PARMS attribute: %m");

        r = sd_netlink_message_append_data(req, TCA_TBF_BURST, &tbf->burst, sizeof(tbf->burst));
        if (r < 0)
                return log_link_error_errno(link, r, "Could not append TCA_TBF_BURST attribute: %m");

        if (tbf->rate >= (1ULL << 32)) {
                r = sd_netlink_message_append_u64(req, TCA_TBF_RATE64, tbf->rate);
                if (r < 0)
                        return log_link_error_errno(link, r, "Could not append TCA_TBF_RATE64 attribute: %m");
        }

        r = sd_netlink_message_append_data(req, TCA_TBF_RTAB, rtab, sizeof(rtab));
        if (r < 0)
                return log_link_error_errno(link, r, "Could not append TCA_TBF_RTAB attribute: %m");

        if (opt.peakrate.rate > 0) {
                if (tbf->peak_rate >= (1ULL << 32)) {
                        r = sd_netlink_message_append_u64(req, TCA_TBF_PRATE64, tbf->peak_rate);
                        if (r < 0)
                                return log_link_error_errno(link, r, "Could not append TCA_TBF_PRATE64 attribute: %m");
                }

                r = sd_netlink_message_append_u32(req, TCA_TBF_PBURST, tbf->mtu);
                if (r < 0)
                        return log_link_error_errno(link, r, "Could not append TCA_TBF_PBURST attribute: %m");

                r = sd_netlink_message_append_data(req, TCA_TBF_PTAB, ptab, sizeof(ptab));
                if (r < 0)
                        return log_link_error_errno(link, r, "Could not append TCA_TBF_PTAB attribute: %m");
        }

        r = sd_netlink_message_close_container(req);
        if (r < 0)
                return log_link_error_errno(link, r, "Could not close container TCA_OPTIONS: %m");

        return 0;
}

int config_parse_token_buffer_filter_size(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        _cleanup_(qdisc_free_or_set_invalidp) QDisc *qdisc = NULL;
        Network *network = data;
        TokenBufferFilter *tbf;
        uint64_t k;
        int r;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = qdisc_new_static(QDISC_KIND_TBF, network, filename, section_line, &qdisc);
        if (r == -ENOMEM)
                return log_oom();
        if (r < 0)
                return log_syntax(unit, LOG_ERR, filename, line, r,
                                  "More than one kind of queueing discipline, ignoring assignment: %m");

        tbf = TBF(qdisc);

        if (isempty(rvalue)) {
                if (streq(lvalue, "Rate"))
                        tbf->rate = 0;
                else if (streq(lvalue, "Burst"))
                        tbf->burst = 0;
                else if (streq(lvalue, "LimitSize"))
                        tbf->limit = 0;
                else if (streq(lvalue, "MTUBytes"))
                        tbf->mtu = 0;
                else if (streq(lvalue, "MPUBytes"))
                        tbf->mpu = 0;
                else if (streq(lvalue, "PeakRate"))
                        tbf->peak_rate = 0;

                qdisc = NULL;
                return 0;
        }

        r = parse_size(rvalue, 1000, &k);
        if (r < 0) {
                log_syntax(unit, LOG_ERR, filename, line, r,
                           "Failed to parse '%s=', ignoring assignment: %s",
                           lvalue, rvalue);
                return 0;
        }

        if (streq(lvalue, "Rate"))
                tbf->rate = k / 8;
        else if (streq(lvalue, "Burst"))
                tbf->burst = k;
        else if (streq(lvalue, "LimitSize"))
                tbf->limit = k;
        else if (streq(lvalue, "MPUBytes"))
                tbf->mpu = k;
        else if (streq(lvalue, "MTUBytes"))
                tbf->mtu = k;
        else if (streq(lvalue, "PeakRate"))
                tbf->peak_rate = k / 8;

        qdisc = NULL;

        return 0;
}

int config_parse_token_buffer_filter_latency(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        _cleanup_(qdisc_free_or_set_invalidp) QDisc *qdisc = NULL;
        Network *network = data;
        TokenBufferFilter *tbf;
        usec_t u;
        int r;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = qdisc_new_static(QDISC_KIND_TBF, network, filename, section_line, &qdisc);
        if (r == -ENOMEM)
                return log_oom();
        if (r < 0)
                return log_syntax(unit, LOG_ERR, filename, line, r,
                                  "More than one kind of queueing discipline, ignoring assignment: %m");

        tbf = TBF(qdisc);

        if (isempty(rvalue)) {
                tbf->latency = 0;

                qdisc = NULL;
                return 0;
        }

        r = parse_sec(rvalue, &u);
        if (r < 0) {
                log_syntax(unit, LOG_ERR, filename, line, r,
                           "Failed to parse '%s=', ignoring assignment: %s",
                           lvalue, rvalue);
                return 0;
        }

        tbf->latency = u;

        qdisc = NULL;

        return 0;
}

static int token_buffer_filter_verify(QDisc *qdisc) {
        TokenBufferFilter *tbf = TBF(qdisc);

        if (tbf->limit > 0 && tbf->latency > 0)
                return log_warning_errno(SYNTHETIC_ERRNO(EINVAL),
                                         "%s: Specifying both LimitSize= and LatencySec= is not allowed. "
                                         "Ignoring [TokenBufferFilter] section from line %u.",
                                         qdisc->section->filename, qdisc->section->line);

        if (tbf->limit == 0 && tbf->latency == 0)
                return log_warning_errno(SYNTHETIC_ERRNO(EINVAL),
                                         "%s: Either LimitSize= or LatencySec= is required. "
                                         "Ignoring [TokenBufferFilter] section from line %u.",
                                         qdisc->section->filename, qdisc->section->line);

        if (tbf->rate == 0)
                return log_warning_errno(SYNTHETIC_ERRNO(EINVAL),
                                         "%s: Rate= is mandatory. "
                                         "Ignoring [TokenBufferFilter] section from line %u.",
                                         qdisc->section->filename, qdisc->section->line);

        if (tbf->burst == 0)
                return log_warning_errno(SYNTHETIC_ERRNO(EINVAL),
                                         "%s: Burst= is mandatory. "
                                         "Ignoring [TokenBufferFilter] section from line %u.",
                                         qdisc->section->filename, qdisc->section->line);

        if (tbf->peak_rate > 0 && tbf->mtu == 0)
                return log_warning_errno(SYNTHETIC_ERRNO(EINVAL),
                                         "%s: MTUBytes= is mandatory when PeakRate= is specified. "
                                         "Ignoring [TokenBufferFilter] section from line %u.",
                                         qdisc->section->filename, qdisc->section->line);

        return 0;
}

const QDiscVTable tbf_vtable = {
        .object_size = sizeof(TokenBufferFilter),
        .tca_kind = "tbf",
        .fill_message = token_buffer_filter_fill_message,
        .verify = token_buffer_filter_verify
};
