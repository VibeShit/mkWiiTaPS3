#pragma once
#include "mkw/effects.hpp"
#include "mkw/kart_smoke_effects.hpp"
#include "mkw/race_effects.hpp"
#include "mkw/vehicle_exhaust.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace mkw {
// The other racers' KartEffect emitters (CPUs and splitscreen players 2-4):
// the ones player 0 gets from its own block in the race loop, i.e. exhaust
// flames and their onset burst, the bike wheelie trail, the small drift
// chips, kart drift smoke, off-road dust and the trick sparkle. Each racer
// owns SlotsPerRacer slots and its KartEffect group (2 + racer), so the
// kart effect LOD masks them with the rest of that racer's effects.
struct RemoteKartFxInput {
    EffectFrame pose;      // kart space: origin at the kart, x/y/z its axes
    Vec3 rearLeft,rearRight;
    unsigned vehicle=0;    // RaceScenario vehicle id, for the exhaust pipes
    bool bike=false;
    bool boosting=false;
    uint16_t mushroomUses=0;
    unsigned driftState=0; // KartMove::driftState: 2 blue, 3 orange
    bool drifting=false;   // a held drift on the ground
    bool wheelie=false;
    bool offroad=false;
    bool trick=false;      // InATrick or ZipperTrick
    bool hidden=false;     // respawning or a Bullet Bill
    bool lod=false;        // the kart effect LOD drops this racer
};

class RemoteKartEffects {
public:
    static constexpr unsigned MaxRacers=12,SlotsPerRacer=16;
    static constexpr unsigned FirstSlot=KartLandingEffects::FirstSlot+12*3;
    static_assert(FirstSlot+(MaxRacers-1)*SlotsPerRacer<=RaceEffects::SlotCount,"remote kart effect slots");
    bool bind(RaceEffects& e){
        flame_=e.find("rk_miniTurbo");burst_=e.find("rk_miniTurbo1T");wheelie_=e.find("rk_wheelie");
        trick_=e.find("rk_jumpSeikou");smoke_=e.find("rk_driftSmoke");dirt_=e.find("rk_dirtSmokeA");
        static constexpr const char* chips[6]={"rk_driftSpark1L_Chip00","rk_driftSpark1R_Chip00","rk_driftSpark2L_Chip00",
            "rk_driftSpark2R_Chip00","rk_driftSpark1LB_Chip00","rk_driftSpark1RB_Chip00"};
        for(unsigned i=0;i<6;++i)chip_[i]=e.find(chips[i]);
        return flame_>=0;
    }
    void reset(){state_={};}
    void step(RaceEffects& e,unsigned racer,const RemoteKartFxInput& in){
        if(!racer||racer>=MaxRacers)return;
        auto& st=state_[racer];
        const unsigned base=FirstSlot+(racer-1)*SlotsPerRacer;
        if(in.hidden||in.lod){
            for(unsigned s=0;s<SlotsPerRacer;++s)e.kill(base+s);
            st={};return;
        }
        const uint8_t group=uint8_t(2+racer);
        const auto& p=in.pose;
        auto kartFrame=[&](Vec3 origin){EffectFrame f=p;f.origin=origin;f.scale=1;return f;};
        // Exhaust flames while any boost runs; each onset and Mushroom use bursts.
        const bool burst=in.boosting&&(!st.boosting||in.mushroomUses!=st.mushroomUses);
        st.boosting=in.boosting;st.mushroomUses=in.mushroomUses;
        const auto& exhaust=VehicleExhausts[std::min<unsigned>(in.vehicle,unsigned(VehicleExhausts.size()-1))];
        for(unsigned i=0;i<4;++i){
            const bool pipe=i<exhaust.count;
            EffectFrame f=kartFrame(p.origin);
            if(pipe){
                const float* x=exhaust.pipes[i];
                const float pitch=x[3]*3.14159265f/180.f,yaw=x[4]*3.14159265f/180.f;
                const float sp=std::sin(pitch),cp=std::cos(pitch),sy=std::sin(yaw),cy=std::cos(yaw);
                auto axis=[&](float ax,float ay,float az){const float y1=ay*cp-az*sp,z1=ay*sp+az*cp;return p.x*(ax*cy+z1*sy)+p.y*y1+p.z*(-ax*sy+z1*cy);};
                f.origin=p.origin+p.x*x[0]+p.y*x[1]+p.z*x[2];f.x=axis(1,0,0);f.y=axis(0,1,0);f.z=axis(0,0,1);f.scale=exhaust.flameScale;
                e.move(base+4+i,f);
                if(burst&&burst_>=0)e.restart(base+4+i,burst_,f,group);
            }
            e.attach(base+i,flame_,f,pipe&&in.boosting,group);
        }
        if(wheelie_>=0)e.attach(base+8,wheelie_,kartFrame(in.rearLeft),in.bike&&in.wheelie,group);
        // Small drift chips at the rear wheels (blue from 2, orange from 3).
        const bool sparks=in.driftState>=2;
        if(in.bike){
            if(chip_[4]>=0)e.attach(base+9,chip_[4],kartFrame(in.rearLeft),sparks,group);
            if(chip_[5]>=0)e.attach(base+10,chip_[5],kartFrame(in.rearLeft),sparks,group);
        }else{
            const int l=chip_[in.driftState>=3?2:0],r=chip_[in.driftState>=3?3:1];
            if(l>=0)e.attach(base+9,l,kartFrame(in.rearLeft),sparks,group);
            if(r>=0)e.attach(base+10,r,kartFrame(in.rearRight),sparks,group);
        }
        if(smoke_>=0){
            e.attach(base+11,smoke_,kartFrame(in.rearLeft),in.drifting&&!in.bike,group);
            e.attach(base+12,smoke_,kartFrame(in.rearRight),in.drifting&&!in.bike,group);
        }
        if(dirt_>=0){
            e.attach(base+13,dirt_,kartFrame(in.rearLeft),in.offroad,group);
            e.attach(base+14,dirt_,kartFrame(in.rearRight),in.offroad&&!in.bike,group);
        }
        // Trick sparkle: restarted on each trick, following the kart.
        EffectFrame trickFrame;trickFrame.origin=p.origin;
        e.move(base+15,trickFrame);
        if(in.trick&&!st.trick&&trick_>=0)e.restart(base+15,trick_,trickFrame,group);
        st.trick=in.trick;
    }
private:
    struct State {bool boosting=false,trick=false;uint16_t mushroomUses=0;};
    std::array<State,MaxRacers> state_{};
    int flame_=-1,burst_=-1,wheelie_=-1,trick_=-1,smoke_=-1,dirt_=-1;
    std::array<int,6> chip_{-1,-1,-1,-1,-1,-1};
};

// The large drift sparks (DriftEffects) of every racer, drawn in one batch:
// player 0 keeps its own DriftEffects; the others get one each here, with
// the kart or bike definitions. draw() merges the live sparks behind one
// spec table: player 0's specs, then the kart ones, then the bike ones.
class RacerSparks {
public:
    static constexpr unsigned MaxRacers=12,SpecsPerSet=7,SpecCount=SpecsPerSet*3;
    static constexpr unsigned DrawCapacity=DriftEffects::DrawCapacity;
    static_assert(SpecsPerSet==std::tuple_size_v<decltype(DriftEffects::specs)>);
    bool load(const char* kartPath,const char* bikePath){
        ready_=kart_.load(kartPath)&&bike_.load(bikePath);
        return ready_;
    }
    bool ready() const {return ready_;}
    // Each race: a racer takes the kart or bike definitions on its first step.
    void reset(){assigned_={};for(auto& r:racers_)r.reset();}
    void step(unsigned racer,unsigned state,Vec3 left,Vec3 right,Vec3 forward,bool bike,Vec3 up){
        if(!ready_||!racer||racer>=MaxRacers)return;
        if(!assigned_[racer]||bikes_[racer]!=bike){
            racers_[racer]=bike?bike_:kart_;racers_[racer].reset();
            bikes_[racer]=bike;assigned_[racer]=true;
        }
        racers_[racer].step(state,left,right,forward,bike?1:2,up);
    }
    // The merged batch; player 0's sparks first, then the others' until full.
    unsigned merge(const DriftEffects& player0){
        if(!ready_)return 0;
        std::copy(player0.specs.begin(),player0.specs.end(),specs_.begin());
        std::copy(kart_.specs.begin(),kart_.specs.end(),specs_.begin()+SpecsPerSet);
        std::copy(bike_.specs.begin(),bike_.specs.end(),specs_.begin()+2*SpecsPerSet);
        unsigned n=0;
        for(const auto& s:player0.sparks)if(s.age<s.life&&n<DrawCapacity)live_[n++]=s;
        for(unsigned i=1;i<MaxRacers;++i)if(assigned_[i])for(const auto& s:racers_[i].sparks)if(s.age<s.life&&n<DrawCapacity){
            live_[n]=s;live_[n].spec+=bikes_[i]?2*SpecsPerSet:SpecsPerSet;++n;
        }
        return count_=n;
    }
    const SparkSpec* specs() const {return specs_.data();}
    const Spark* sparks() const {return live_.data();}
    unsigned count() const {return count_;}
private:
    DriftEffects kart_,bike_;
    std::array<DriftEffects,MaxRacers> racers_{};
    std::array<bool,MaxRacers> bikes_{},assigned_{};
    std::array<SparkSpec,SpecCount> specs_{};
    std::array<Spark,DrawCapacity> live_{};
    unsigned count_=0;
    bool ready_=false;
};
}
