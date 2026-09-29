#include "o3ds/model.h"
#include "o3ds/udp_fragment.h"
#include "o3ds/reorder_gate.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
using namespace O3DS;

static std::vector<char> frag(uint32_t id, uint32_t seq, uint32_t total, uint32_t fs, size_t payload){
  std::vector<char> v(16+payload, 'x'); uint32_t h[4]={id,seq,total,fs}; memcpy(v.data(),h,16); return v; }

int main(int argc, char** argv){
  std::string which = argc>1 ? argv[1] : "";
  if (which=="calc") {
    // Transform with Component_Matrix but no matrix vector -> CalcMatrices indexes matrices[0]
    flatbuffers::FlatBufferBuilder b;
    std::vector<int8_t> comps{O3DS::Data::Component_Matrix};
    auto n = O3DS::Data::CreateTransformDirect(b, -1, "root", nullptr, nullptr, nullptr, nullptr, &comps);
    std::vector<flatbuffers::Offset<O3DS::Data::Transform>> nodes{n};
    auto s = O3DS::Data::CreateSubjectDirect(b, &nodes, "evil");
    std::vector<flatbuffers::Offset<O3DS::Data::Subject>> subs{s};
    b.Finish(O3DS::Data::CreateSubjectListDirect(b, &subs));
    std::vector<char> out; finalize(b, out, 1);
    SubjectList l; bool ok = l.Parse(out.data(), out.size());
    printf("parse=%d\n", ok);
  } else if (which=="cycle") {
    SubjectList l; auto* s = l.addSubject("c");
    s->addTransform("root",-1); s->addTransform("a",2); s->addTransform("b",1);
    std::vector<char> out; l.Serialize(out, 1.0);
    SubjectList r; bool ok = r.Parse(out.data(), out.size());
    printf("cycle parse=%d err='%s' a.bWorld=%d\n", ok, r.mError.c_str(), r.mItems[0]->mTransforms[1]->bWorldMatrix);
  } else if (which=="frag") {
    UdpMapper m;
    auto f1 = frag(7,0,2000,1000,1000); m.addFragment(f1.data(), f1.size());
    auto f2 = frag(7,1999,2000,1,1);  m.addFragment(f2.data(), f2.size()); // mFound has 2 entries; writes mFound[15]
    printf("frag done found.size=%zu\n", m.items[7].mFound.size());
  } else if (which=="empty") {
    UdpMapper m;
    auto f1 = frag(5,0,2000,1000,1000); m.addFragment(f1.data(), f1.size());
    auto bad = frag(9,0,0,1000,0); bool r = m.addFragment(bad.data(), bad.size()); // rejected by combiner (bufSz==0)
    std::vector<char> out; bool got = m.getFrame(out);
    printf("addFragment(bad)=%d getFrame=%d size=%zu remaining items=%zu\n", r, got, out.size(), m.items.size());
  } else if (which=="dos") {
    UdpMapper m; size_t n=0;
    for (uint32_t id=0; id<200; ++id){ auto f=frag(id,0,64u*1024*1024,65000,65000); if(m.addFragment(f.data(),f.size())) n++; }
    size_t bytes=0; for (auto& kv:m.items) bytes+=kv.second.mBufferSize;
    printf("combiners=%zu reserved=%.1f GB\n", m.items.size(), bytes/1e9);
  } else if (which=="gate") {
    ReorderGate g; int emitted=0; auto emit=[&](Frame&&){emitted++;};
    for (uint64_t s=1;s<=10;++s){ Frame f; f.seq=s; f.epoch=100; g.Push(std::move(f), s*0.016, emit);} 
    Frame bad; bad.seq=1ull<<40; bad.epoch=100; g.Push(std::move(bad), 0.2, emit);
    g.Flush(1.0, emit);
    int before=emitted;
    for (uint64_t s=11;s<=100;++s){ Frame f; f.seq=s; f.epoch=100; g.Push(std::move(f), 1.0+s*0.016, emit);} 
    printf("after forged seq: legit frames delivered=%d of 90, stale=%llu lost=%llu\n", emitted-before, (unsigned long long)g.Stats().stale_dropped, (unsigned long long)g.Stats().lost);
  } else if (which=="residual") {
    // sender with Linear residual, keyframe interval 300; drop one update; measure receiver error
    SubjectList tx; auto* s = tx.addSubject("r"); s->addTransform("root",-1); s->mTransforms[0]->transformOrder={TTranslation,TRotation};
    s->SetResidualEncoder(std::make_unique<ResidualEncoder>(ResidualPredictorId::Linear, 300));
    std::vector<char> buf; s->Serialize(buf, 1.0);
    SubjectList rx; rx.Parse(buf.data(), buf.size());
    double maxErr=0, errAt[4]={0};
    for (int f=1; f<=200; ++f){
      double t=1.0+f/60.0; double x = 50*std::sin(t*3.0);
      double ang = 0.5*std::sin(t*2.0);
      s->mTransforms[0]->translation.value = Vector3d(x,0,0);
      s->mTransforms[0]->rotation.value = Vector4d(0,0,std::sin(ang/2),std::cos(ang/2));
      size_t c=0; std::vector<char> ub; s->SerializeUpdateResidual(ub,c,0.0001,t);
      if (f==50) continue; // one lost packet
      rx.Parse(ub.data(), ub.size(), nullptr, false);
      double e = std::fabs(rx.mItems[0]->mTransforms[0]->translation.value.v[0]-x);
      if (f>50 && e>maxErr) maxErr=e;
      if (f==49) errAt[0]=e; if(f==51) errAt[1]=e; if(f==100) errAt[2]=e; if(f==200) errAt[3]=e;
    }
    auto q = rx.mItems[0]->mTransforms[0]->rotation.value; double len=std::sqrt(q.v[0]*q.v[0]+q.v[1]*q.v[1]+q.v[2]*q.v[2]+q.v[3]*q.v[3]);
    printf("err f49=%g f51=%g f100=%g f200=%g max=%g quatLen=%g\n", errAt[0],errAt[1],errAt[2],errAt[3],maxErr,len);
  } else if (which=="midjoin") {
    SubjectList tx; auto* s = tx.addSubject("r"); s->addTransform("root",-1); s->mTransforms[0]->transformOrder={TTranslation,TRotation};
    s->SetResidualEncoder(std::make_unique<ResidualEncoder>(ResidualPredictorId::Linear, 300));
    std::vector<char> full; s->Serialize(full, 1.0);
    SubjectList rx; bool joined=false; double e=0;
    for (int f=1; f<=120; ++f){
      double t=1.0+f/60.0; double x=100+10*t; s->mTransforms[0]->translation.value=Vector3d(x,0,0);
      size_t c=0; std::vector<char> ub; s->SerializeUpdateResidual(ub,c,0.0001,t);
      if (f==30){ rx.Parse(full.data(), full.size()); joined=true; continue; } // late join: receives descriptor (e.g. reliable resend)
      if (joined){ rx.Parse(ub.data(), ub.size(), nullptr, false); e=std::fabs(rx.mItems[0]->mTransforms[0]->translation.value.v[0]-x);
        if (f==31||f==32||f==60||f==120) printf("f%d err=%g\n", f, e);} }
  } else if (which=="quatres") {
    // residual rotation reconstruction is not renormalized: feed a sign flip
    SubjectList tx; auto* s = tx.addSubject("q"); s->addTransform("root",-1); s->mTransforms[0]->transformOrder={TRotation};
    s->SetResidualEncoder(std::make_unique<ResidualEncoder>(ResidualPredictorId::Linear, 0));
    std::vector<char> full; s->Serialize(full, 1.0); SubjectList rx; rx.Parse(full.data(), full.size());
    for (int f=1; f<=6; ++f){ double t=1+f/60.0; double a=0.3+0.01*f; double sg = (f==4)?-1:1;
      s->mTransforms[0]->rotation.value = Vector4d(0,0,sg*std::sin(a/2),sg*std::cos(a/2));
      size_t c=0; std::vector<char> ub; s->SerializeUpdateResidual(ub,c,0.0001,t); rx.Parse(ub.data(),ub.size(),nullptr,false);
      auto q=rx.mItems[0]->mTransforms[0]->rotation.value; printf("f%d bytes=%zu rx q=(%.3f %.3f %.3f %.3f) len=%.4f\n", f, ub.size(), q.v[0],q.v[1],q.v[2],q.v[3], std::sqrt(q.v[2]*q.v[2]+q.v[3]*q.v[3]+q.v[0]*q.v[0]+q.v[1]*q.v[1])); }
  }
  return 0;
}
