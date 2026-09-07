#include "midi_harmony.h"
void MidiChordState::note_on(uint8_t n,uint8_t v){ if(n<128){held_[n].store(v?1:0,std::memory_order_release);generation_.fetch_add(1,std::memory_order_release);} }
void MidiChordState::note_off(uint8_t n){if(n<128){held_[n].store(0,std::memory_order_release);generation_.fetch_add(1,std::memory_order_release);}}
void MidiChordState::all_notes_off(){for(auto &n:held_)n.store(0,std::memory_order_release);generation_.fetch_add(1,std::memory_order_release);}
std::array<bool,128> MidiChordState::snapshot() const {std::array<bool,128> r{};for(std::size_t i=0;i<r.size();++i)r[i]=held_[i].load(std::memory_order_acquire)!=0;return r;}
