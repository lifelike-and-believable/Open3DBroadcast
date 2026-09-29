#include "o3ds/model.h"
#include "CRC.h"
#include <chrono>
#include <cstdio>
using namespace O3DS;
int main(){
  SubjectList l; auto* s=l.addSubject("MetaHuman");
  for (int i=0;i<250;i++){ auto* t=s->addTransform("bone_"+std::to_string(i), i-1); t->transformOrder={TTranslation,TRotation,TScale}; }
  for (int i=0;i<250;i++){ s->mCurveNames.push_back("CTRL_expressions_curve_"+std::to_string(i)); s->mCurveValues.push_back(0.1f*i);} 
  std::vector<char> out; const int N=2000;
  auto t0=std::chrono::steady_clock::now(); for(int i=0;i<N;i++) l.Serialize(out,1.0+i);
  double ser=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-t0).count()/N;
  SubjectList r; t0=std::chrono::steady_clock::now(); for(int i=0;i<N;i++) r.Parse(out.data(),out.size());
  double par=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-t0).count()/N;
  t0=std::chrono::steady_clock::now(); volatile uint32_t c=0; for(int i=0;i<N;i++) c+=CRCPP::CRC::Calculate(out.data()+8,out.size()-8,CRCPP::CRC::CRC_32());
  double crc=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-t0).count()/N;
  static const CRCPP::CRC::Table<uint32_t,32> tbl(CRCPP::CRC::CRC_32());
  t0=std::chrono::steady_clock::now(); for(int i=0;i<N;i++) c+=CRCPP::CRC::Calculate(out.data()+8,out.size()-8,tbl);
  double crct=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-t0).count()/N;
  printf("bytes=%zu serialize=%.1fus parse=%.1fus crc_bitwise=%.1fus crc_table=%.1fus\n", out.size(), ser, par, crc, crct);
}
