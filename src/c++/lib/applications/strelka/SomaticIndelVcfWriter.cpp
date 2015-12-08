// -*- mode: c++; indent-tabs-mode: nil; -*-
//
// Strelka - Small Variant Caller
// Copyright (c) 2009-2016 Illumina, Inc.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
//

/// \author Chris Saunders
///

#include "SomaticIndelVcfWriter.hh"
#include "somatic_call_shared.hh"
#include "somatic_indel_grid.hh"
#include "somatic_indel_vqsr_features.hh"
#include "strelka_vcf_locus_info.hh"
#include "blt_util/blt_exception.hh"
#include "blt_util/io_util.hh"
#include "blt_util/qscore.hh"
#include "blt_util/fisher_exact_test.hh"
#include "blt_util/binomial_test.hh"

#include <iomanip>
#include <iostream>
#include <limits>
#include <calibration/scoringmodels.hh>


static
void
write_vcf_isri_tiers(
    const starling_indel_sample_report_info& isri1,
    const starling_indel_sample_report_info& isri2,
    const win_avg_set& was,
    std::ostream& os)
{
    static const char sep(':');
//  DP:DP2:TAR:TIR:TOR...
    os << isri1.depth
       << sep
       << isri2.depth
       << sep
       << isri1.n_q30_ref_reads+isri1.n_q30_alt_reads << ','
       << isri2.n_q30_ref_reads+isri2.n_q30_alt_reads
       << sep
       << isri1.n_q30_indel_reads << ','
       << isri2.n_q30_indel_reads
       << sep
       << isri1.n_other_reads << ','
       << isri2.n_other_reads;

    // AF:OF:SOR:FS:BSA:RR:BCN
    const StreamScoper ss(os);
    os << std::fixed << std::setprecision(3);
    os << sep << calculateIndelAF(isri1)
              << sep << calculateIndelOF(isri1)
              << sep << calculateSOR(isri1)
              << sep << calculateFS(isri1)
              << sep << calculateBSA(isri1)
              << sep << isri1.readpos_ranksum.get_u_stat()
              << sep << calculateBCNoise(was)
            ;
}



static
void
writeSomaticIndelVcfGrid(
    const strelka_options& opt,
    const strelka_deriv_options& dopt,
    const pos_t pos,
    const SomaticIndelVcfInfo& siInfo,
    const win_avg_set& wasNormal,
    const win_avg_set& wasTumor,
    std::ostream& os)
{
    const somatic_indel_call::result_set& rs(siInfo.sindel.rs);

    // always use high depth filter when enabled
    strelka_shared_modifiers_indel smod;
    if (dopt.sfilter.is_max_depth())
    {
        const unsigned& depth(siInfo.nisri[0].depth);
        if (depth > dopt.sfilter.max_depth)
        {
            smod.set_filter(STRELKA_VCF_FILTERS::HighDepth);
        }
    }

    // calculate VQSR score and features
    calculateVQSRFeatures(siInfo, wasNormal, wasTumor, opt, dopt, smod);

    // always write VQSR feature
    const scoring_models& models(scoring_models::Instance());
    if(models.isVariantScoringInit())
    {
        // for some reason these come out the wrong way around
        smod.Qscore = 1.0 - models.score_variant(smod.get_features(), VARIATION_NODE_TYPE::INDEL);
        smod.isQscore = true;
    }
    else
    {
        smod.Qscore = 0;
    }

    const bool is_use_empirical_scoring(opt.sfilter.is_use_indel_empirical_scoring);
    if (!is_use_empirical_scoring) {
        // compute all site filters:
        const double normalWinFrac = calculateBCNoise(wasNormal);
        const double tumorWinFrac = calculateBCNoise(wasTumor);

        if ((normalWinFrac >= opt.sfilter.indelMaxWindowFilteredBasecallFrac) ||
            (tumorWinFrac >= opt.sfilter.indelMaxWindowFilteredBasecallFrac))
        {
            smod.set_filter(STRELKA_VCF_FILTERS::IndelBCNoise);
        }

        if ((rs.ntype != NTYPE::REF) || (rs.sindel_from_ntype_qphred < opt.sfilter.sindelQuality_LowerBound))
        {
            smod.set_filter(STRELKA_VCF_FILTERS::QSI_ref);
        }
    }
    else
    {
        if (rs.ntype != NTYPE::REF)
        {
            smod.set_filter(STRELKA_VCF_FILTERS::Nonref);
        }

        if(smod.Qscore < models.score_threshold(VARIATION_NODE_TYPE::INDEL))
        {
            smod.set_filter(STRELKA_VCF_FILTERS::LowQscore);
        }
    }

    const pos_t output_pos(pos+1);

    static const char sep('\t');
    // CHROM
    os << opt.bam_seq_name;

    // POS+
    os << sep << output_pos;

    // ID
    os << sep << ".";

    // REF/ALT
    os << sep << siInfo.iri.vcf_ref_seq
       << sep << siInfo.iri.vcf_indel_seq;

    //QUAL:
    os << sep << ".";

    //FILTER:
    os << sep;
    smod.write_filters(os);

    //INFO
    os << sep
       << "SOMATIC";

    if(smod.isQscore)
    {
        const StreamScoper ss(os);
        os << std::fixed << std::setprecision(4);
        os << ";EQSI=" << smod.Qscore;
    }

    os << ";QSI=" << rs.sindel_qphred
       << ";TQSI=" << (siInfo.sindel.sindel_tier+1)
       << ";NT=" << NTYPE::label(rs.ntype)
       << ";QSI_NT=" << rs.sindel_from_ntype_qphred
       << ";TQSI_NT=" << (siInfo.sindel.sindel_from_ntype_tier+1)
       << ";SGT=" << static_cast<DDIINDEL_GRID::index_t>(rs.max_gt);

    {
        const StreamScoper ss(os);
        double mean_mapq  = (siInfo.nisri[1].mean_mapq + siInfo.tisri[1].mean_mapq) / 2.0;
        double mean_mapq0 = (siInfo.nisri[1].mapq0_frac*siInfo.nisri[1].n_mapq +
                             siInfo.tisri[1].mapq0_frac*siInfo.tisri[1].n_mapq) / (
                                    siInfo.nisri[1].n_mapq + siInfo.tisri[1].n_mapq);
        os << std::fixed << std::setprecision(2);
        os << ";MQ=" << mean_mapq
           << ";MQ0=" << mean_mapq0
                ;
    }
    if (siInfo.iri.is_repeat_unit())
    {
        os << ";RU=" << siInfo.iri.repeat_unit
           << ";RC=" << siInfo.iri.ref_repeat_count
           << ";IC=" << siInfo.iri.indel_repeat_count;
    }
    os << ";IHP=" << siInfo.iri.ihpol;

    if(is_use_empirical_scoring)
    {
        os << ";ESF=";
        smod.write_features(os);
    }

    if ((siInfo.iri.it == INDEL::BP_LEFT) ||
        (siInfo.iri.it == INDEL::BP_RIGHT))
    {
        os << ";SVTYPE=BND";
    }

    if (rs.is_overlap)
    {
        os << ";OVERLAP";
    }


    //FORMAT
    os << sep << "DP:DP2:TAR:TIR:TOR:AF:OF:SOR:FS:BSA:RR";
    os << ":BCN" << opt.sfilter.indelRegionFlankSize;

    // write normal sample info:
    os << sep;
    write_vcf_isri_tiers(siInfo.nisri[0],siInfo.nisri[1], wasNormal,os);

    // write tumor sample info:
    os << sep;
    write_vcf_isri_tiers(siInfo.tisri[0],siInfo.tisri[1], wasTumor,os);

    os << '\n';
}


void
SomaticIndelVcfWriter::
cacheIndel(
    const pos_t pos,
    const SomaticIndelVcfInfo& siInfo)
{
#if 0
    if (_data.count(pos) != 0)
    {
        std::ostringstream oss;
        oss << "ERROR: Attempting to cache 2 indels at one site.\n";
        oss << "\texisting indel REF/ALT:\t" << siInfo.iri.ref_seq << " " << siInfo.iri.indel_seq << "\n";
        oss << "\tnew indel REF/ALT:\t" << _data[pos].iri.ref_seq << " " << _data[pos].iri.indel_seq << "\n";
        throw blt_exception(oss.str().c_str());
    }
#endif
    _data[pos].push_back(siInfo);
}



void
SomaticIndelVcfWriter::
addIndelWindowData(
    const pos_t pos,
    const win_avg_set& wasNormal,
    const win_avg_set& wasTumor)
{
    assert(testPos(pos));
    for (const auto& indelInfo : _data[pos])
    {
        writeSomaticIndelVcfGrid(_opt, _dopt, pos, indelInfo, wasNormal, wasTumor, *_osptr);
    }
    _data.erase(pos);
}
