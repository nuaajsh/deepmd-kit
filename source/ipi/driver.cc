#include <fstream>
#include <cstdint>
#include "sockets.h"
#include "NNPInter.h"
#include "Convert.h"
#include "XyzFileManager.h"
#include "SimulationRegion.h"

#include "json.hpp"
using json = nlohmann::json;


using namespace std;

// bohr -> angstrom
const double cvt_len  = 0.52917721;
const double icvt_len = 1./cvt_len;
// hatree -> eV
const double cvt_ener  = 27.21138602;
const double icvt_ener = 1./cvt_ener;
// hatree/Bohr -> eV / angstrom
const double cvt_f  = cvt_ener / cvt_len;
const double icvt_f = 1./cvt_f;

char *trimwhitespace(char *str)
{
  char *end;
  // Trim leading space
  while(isspace((unsigned char)*str)) str++;
  if(*str == 0)  // All spaces?
    return str;
  // Trim trailing space
  end = str + strlen(str) - 1;
  while(end > str && isspace((unsigned char)*end)) end--;
  // Write new null terminator
  *(end+1) = 0;
  return str;
}

void 
normalize_coord (vector<VALUETYPE > & coord,
		 const SimulationRegion<VALUETYPE > & region)
{
  int natoms = coord.size() / 3;

  for (int ii = 0; ii < natoms; ++ii){
    VALUETYPE inter[3];
    region.phys2Inter (inter, &coord[3*ii]);
    for (int dd = 0; dd < 3; ++dd){
      inter[dd] -= int(floor(inter[dd]));
      if      (inter[dd] < 0 ) inter[dd] += 1.;
      else if (inter[dd] >= 1) inter[dd] -= 1.;
    }
    region.inter2Phys (&coord[3*ii], inter);
  }
}

int main(int argc, char * argv[])
{
  if (argc == 1) {
    cerr << "usage " << endl;
    cerr << argv[0] << " input_script " << endl;
    return 1;
  }

  ifstream fp (argv[1]);
  json jdata;
  fp >> jdata;
  cout << "# using data base" << endl;
  cout << setw(4) << jdata << endl;

  int socket;
  int inet = 1;
  if (jdata["use_unix"]) {
    inet = 0;
  }
  int port = jdata["port"];
  string host_str = jdata["host"];
  const char * host = host_str.c_str();
  string graph_file = jdata["graph_file"];
  string coord_file = jdata["coord_file"];
  map<string, int> name_type_map = jdata["atom_type"];
  bool b_verb = jdata["verbose"];
  
  vector<string > atom_name;  
  {
    vector<vector<double > >  posi;
    vector<vector<double > >  velo;
    vector<vector<double > >  forc;
    XyzFileManager::read (coord_file, atom_name, posi, velo, forc);
  }

  Convert<double> cvt (atom_name, name_type_map);
  NNPInter nnp_inter (graph_file);
  
  enum { _MSGLEN = 12 };
  int MSGLEN = _MSGLEN;
  char header [_MSGLEN+1] = {'\0'};
  bool hasdata = false;
  int32_t cbuf = 0;
  char initbuffer[2048];
  double cell_h [9];
  double cell_ih[9];
  int32_t natoms = -1;
  double dener (0);
  vector<double > dforce;
  vector<double > dforce_tmp;
  vector<double > dvirial (9, 0);
  vector<double > dcoord ;
  vector<double > dcoord_tmp ;
  vector<int > dtype = cvt.get_type();
  vector<double > dbox (9, 0) ;
  SimulationRegion<double > region;
  double * msg_buff = NULL;
  double ener;
  double virial[9];
  char msg_needinit[]	= "NEEDINIT    ";
  char msg_havedata[]	= "HAVEDATA    ";
  char msg_ready[]	= "READY       ";
  char msg_forceready[] = "FORCEREADY  ";
  char msg_nothing[]	= "nothing";
  
  open_socket_ (&socket, &inet, &port, host);
  
  bool isinit = true;

  while (true) {
    readbuffer_ (&socket, header, MSGLEN);
    string header_str (trimwhitespace(header));
    if (b_verb) cout << "# get header " << header_str << endl;

    if (header_str == "STATUS"){
      if (! isinit) {
	writebuffer_ (&socket, msg_needinit, MSGLEN);
	if (b_verb) cout << "# send back  " << "NEEDINIT" << endl;
      }
      else if (hasdata) {
	writebuffer_ (&socket, msg_havedata, MSGLEN);
	if (b_verb) cout << "# send back  " << "HAVEDATA" << endl;
      }
      else {
	writebuffer_ (&socket, msg_ready, MSGLEN);
	if (b_verb) cout << "# send back  " << "READY" << endl;
      }
    }
    else if (header_str == "INIT") {
      assert (4 == sizeof(int32_t));
      readbuffer_ (&socket, (char *)(&cbuf), sizeof(int32_t));
      readbuffer_ (&socket, initbuffer, cbuf);
      if (b_verb) cout << "Init sys from wrapper, using " << initbuffer << endl;
    }
    else if (header_str == "POSDATA"){
      assert (8 == sizeof(double));
      
      // get box
      readbuffer_ (&socket, (char *)(cell_h),  9*sizeof(double));
      readbuffer_ (&socket, (char *)(cell_ih), 9*sizeof(double));
      for (int dd = 0; dd < 9; ++dd){
	dbox[dd] = cell_h[dd] * cvt_len;
      }
      region.reinitBox (&dbox[0]);
      
      // get number of atoms
      readbuffer_ (&socket, (char *)(&cbuf), sizeof(int32_t));
      if (natoms < 0) {
	natoms = cbuf;
	if (b_verb) cout << "# get number of atoms in system: " << natoms << endl;
	
	dcoord.resize (3 * natoms);
	dforce.resize (3 * natoms, 0);
	dcoord_tmp.resize (3 * natoms);
	dforce_tmp.resize (3 * natoms, 0);
	msg_buff = new double [3 * natoms];
      }
      
      // get coord
      readbuffer_ (&socket, (char *)(msg_buff), natoms * 3 * sizeof(double));
      for (int ii = 0; ii < natoms * 3; ++ii){
	dcoord_tmp[ii] = msg_buff[ii] * cvt_len;
      }
      cvt.forward (dcoord, dcoord_tmp, 3);
      normalize_coord (dcoord, region);

      // nnp over writes ener, force and virial
      nnp_inter.compute (dener, dforce_tmp, dvirial, dcoord, dtype, dbox);   
      cvt.backward (dforce, dforce_tmp, 3);
      hasdata = true;
    }
    else if (header_str == "GETFORCE"){
      ener = dener * icvt_ener;
      for (int ii = 0; ii < natoms * 3; ++ii){
	msg_buff[ii] = dforce[ii] * icvt_f;
      }
      for (int ii = 0; ii < 9; ++ii){
	virial[ii] = dvirial[ii] * icvt_ener * (1.0);
      }
      if (b_verb) cout << "# energy of sys. : " << scientific << setprecision(10) << dener << endl;
      writebuffer_ (&socket, msg_forceready, MSGLEN);
      writebuffer_ (&socket, (char *)(&ener), sizeof(double));
      writebuffer_ (&socket, (char *)(&natoms), sizeof(int32_t));
      writebuffer_ (&socket, (char *)(msg_buff), 3 * natoms * sizeof(double));
      writebuffer_ (&socket, (char *)(virial), 9 * sizeof(double));
      cbuf = 7;
      writebuffer_ (&socket, (char *)(&cbuf), sizeof(int32_t));
      writebuffer_ (&socket, msg_nothing, 7);
      hasdata = false;
    }
    else {
      cerr << "unexpected header " << endl;
      return 1;
    }
  }

  if (msg_buff != NULL){
    delete [] msg_buff;
  }
}
