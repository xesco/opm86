// The YM2151's FM engine, built here in full: MaskedYm2151 (SongPlayer.h)
// calls its clock and output directly, and ymfm_opm.cpp only builds the
// members its own code uses, which a compiler may inline away.
#include "ymfm_opm.h"
#include "ymfm_fm.ipp"

template class ymfm::fm_engine_base<ymfm::opm_registers>;
