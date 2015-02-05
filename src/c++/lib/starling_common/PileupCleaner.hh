// -*- mode: c++; indent-tabs-mode: nil; -*-
//
// Starka
// Copyright (c) 2009-2014 Illumina, Inc.
//
// This software is provided under the terms and conditions of the
// Illumina Open Source Software License 1.
//
// You should have received a copy of the Illumina Open Source
// Software License 1 along with this program. If not, see
// <https://github.com/sequencing/licenses/>
//

///
/// \author Chris Saunders
///

#pragma once

#include "blt_common/adjust_joint_eprob.hh"
#include "blt_common/blt_shared.hh"

#include <cassert>


/// filtered pileup with processed qualities and summary stats
struct CleanedPileup
{
    friend struct PileupCleaner;

    CleanedPileup()
        : _epi(cleanedPileup(),dependentErrorProb())
    {}

    unsigned
    n_calls() const
    {
        // pre-computed to reflect tier1/tier2
        return _n_raw_calls;
    }

    unsigned
    n_used_calls() const
    {
        return _cleanedPileup.calls.size();
    }

    unsigned
    n_unused_calls() const
    {
        return n_calls()-n_used_calls();
    }

    const snp_pos_info&
    rawPileup() const
    {
        assert(nullptr != _rawPileupPtr);
        return (*_rawPileupPtr);
    }

    const snp_pos_info&
    cleanedPileup() const
    {
        return _cleanedPileup;
    }

    const std::vector<float>&
    dependentErrorProb() const
    {
        return _dependentErrorProb;
    }

    /// deprecated, many old functions ask for this object, so this
    /// eases the transition
    const extended_pos_info&
    getExtendedPosInfo() const
    {
        return _epi;
    }

    void
    clear()
    {
        _rawPileupPtr = nullptr;
        _n_raw_calls = 0;
        _cleanedPileup.clear();
        _dependentErrorProb.clear();
    }

private:
    snp_pos_info&
    cleanedPileup()
    {
        return _cleanedPileup;
    }

    std::vector<float>&
    dependentErrorProb()
    {
        return _dependentErrorProb;
    }

    const snp_pos_info* _rawPileupPtr = nullptr;
    unsigned _n_raw_calls = 0;
    snp_pos_info _cleanedPileup;
    std::vector<float> _dependentErrorProb;
    const extended_pos_info _epi;
};


/// takes raw single sample pileup and processes information so that
/// it meets the criteria for snp calling
struct PileupCleaner
{
    PileupCleaner(
        const blt_options& opt)
        : _opt(opt)
    {}

    void
    CleanPileupFilter(
        const snp_pos_info& pi,
        const bool is_include_tier2,
        CleanedPileup& cpi) const;

    void
    CleanPileupErrorProb(
        CleanedPileup& cpi) const;

    void
    CleanPileup(
        const snp_pos_info& pi,
        const bool is_include_tier2,
        CleanedPileup& cpi) const
    {
        CleanPileupFilter(pi, is_include_tier2, cpi);
        CleanPileupErrorProb(cpi);
    }

private:
    const blt_options& _opt;
    mutable dependent_prob_cache _dpcache;
};