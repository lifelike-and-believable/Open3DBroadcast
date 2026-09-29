#include "o3ds/model.h"
#include <chrono>
#include <cstdio>
using namespace O3DS;
int main(){ for (int N : {2000, 8000, 16000}) {
  SubjectList l; auto* s=l.addSubject("c");
  // node i's parent is i+1 (reverse order), last node is root
  for (int i=0;i<N;i++) s->addTransform("n"+std::to_string(i), i==N-1?-1:i+1);
  std::vector<char> out; l.Serialize(out,1.0);
  SubjectList r; auto t0=std::chrono::steady_clock::now(); r.Parse(out.data(), out.size());
  double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t0).count();
  printf("N=%d bytes=%zu parse_ms=%.1f\n", N, out.size(), ms);} }
