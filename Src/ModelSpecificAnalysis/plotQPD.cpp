#include <string>
#include <iostream>
#include <set>

#include <AMReX_ParmParse.H>
#include <AMReX_MultiFab.H>
#include <AMReX_DataServices.H>
#include <AMReX_BCRec.H>
#include <AMReX_Interpolater.H>
#include <AMReX_GpuLaunch.H>
#include <AMReX_WritePlotFile.H>

#include <PelePhysics.H>

using namespace amrex;
using namespace analysis_util;

typedef std::list<Edge> EdgeList;

static
void 
print_usage (int,
             char* argv[])
{
  std::cerr << "usage:\n";
  std::cerr << argv[0] << " infile infile=f1 [options] \n\tOptions:\n";
  exit(1);
}

struct ELIcompare
{
  // Order EL iters using the underlying edge
  bool operator()(const EdgeList::const_iterator& lhs, const EdgeList::const_iterator& rhs) {
    return *lhs < *rhs;
  }
};

std::string
getFileRoot(const std::string& infile)
{
  vector<std::string> tokens = Tokenize(infile,std::string("/"));
  return tokens[tokens.size()-1];
}

int
main (int   argc,
      char* argv[])
{
  Initialize(argc,argv);
  {
    if (argc < 2)
      print_usage(argc,argv);

    ParmParse pp;

    if (pp.contains("help"))
      print_usage(argc,argv);

    if (pp.contains("verbose"))
      AmrData::SetVerbose(true);

    std::string plotFileName; pp.get("infile",plotFileName);
    DataServices::SetBatchMode();
    Amrvis::FileType fileType(Amrvis::NEWPLT);

    DataServices dataServices(plotFileName, fileType);
    if( ! dataServices.AmrDataOk()) {
      DataServices::Dispatch(DataServices::ExitRequest, NULL);
      // ^^^ this calls ParallelDescriptor::EndParallel() and exit()
    }
    AmrData& amrData = dataServices.AmrDataRef();

    int finestLevel = amrData.FinestLevel();
    pp.query("finestLevel",finestLevel);
    int Nlev = finestLevel + 1;

    int idXin = -1;
    int idTin = -1;
    int idRin = -1;
    Vector<std::string> spec_names = GetSpecNames();
    const Vector<std::string>& plotVarNames = amrData.PlotVarNames();
    const std::string spName= "X(" + spec_names[0] + ")";
    const std::string TName = "temp";
    const std::string RName = "density";
    for (int i=0; i<plotVarNames.size(); ++i)
    {
      if (plotVarNames[i] == spName) idXin = i;
      if (plotVarNames[i] == TName) idTin = i;
      if (plotVarNames[i] == RName) idRin = i;
    }
    if (idXin<0 || idTin<0 || idRin<0)
      Print() << "Cannot find required data in pltfile" << std::endl;

    int nspecies = NumSpecies();
    int nreactions = NumReactions();
    int nCompIn = nspecies + 2;
    Vector<std::string> inNames(nCompIn);
    Vector<int> destFillComps(nCompIn);
    const int idXlocal = 0; // Xs start here
    const int idTlocal = nspecies;
    const int idRlocal = nspecies+1;
    for (int i=0; i<nspecies; ++i)
    {
      destFillComps[i] = idXlocal + i;
      inNames[i] =  "X(" + spec_names[i] + ")";
    }
    destFillComps[idTlocal] = idTlocal;
    destFillComps[idRlocal] = idRlocal;
    inNames[idTlocal] = TName;
    inNames[idRlocal] = RName;
    
    Vector<Real> Qfsum(nreactions,0), Qrsum(nreactions,0);
    
    const int nGrow = 0;
    for (int lev=0; lev<Nlev; ++lev)
    {
      const BoxArray ba = amrData.boxArray(lev);
      const DistributionMapping dm(ba);
      MultiFab indata(ba,dm,nCompIn,nGrow);

      Print() << "Reading data for level " << lev << std::endl;
      amrData.FillVar(indata,lev,inNames,destFillComps);
      Print() << "Data has been read for level " << lev << std::endl;

      MultiFab Qf(ba,dm,nreactions,nGrow);
      MultiFab Qr(ba,dm,nreactions,nGrow);

      // NOTE: 
#ifdef _OPENMP
      Abort("OMP threading currently broken");
//#pragma omp parallel
#endif
      for (MFIter mfi(indata,TilingIfNotGPU()); mfi.isValid(); ++mfi)
      {
        const Box& bx = mfi.tilebox();
        Array4<Real> const& inarr = indata.array(mfi);
        //Array4<Real> const& Qarr = Q.array(mfi);
        Array4<Real> const& Qfarr = Qf.array(mfi);
        Array4<Real> const& Qrarr = Qr.array(mfi);

        AMREX_PARALLEL_FOR_3D ( bx, i, j, k,
        {
          Real Xl[nspecies];
          Real Tl = inarr(i,j,k,idTlocal);
          Real Rl = inarr(i,j,k,idRlocal) * 1.e-3;
          for (int n=0; n<nspecies; ++n) {
            Xl[n] = inarr(i,j,k,idXlocal+n);
          }

          Real Qf[nreactions];
          Real Qr[nreactions];
          Real Pcgs;
          CKPX(&Rl,&Tl,Xl,&Pcgs);
          CKKFKR(&Pcgs,&Tl,Xl,Qf,Qr);

          for (int n=0; n<nreactions; ++n) {
            Qfarr(i,j,k,n) = Qf[n];
            Qrarr(i,j,k,n) = Qr[n];
          }
        });
        
        // Zero out covered data
        if (lev < finestLevel){
          BoxArray baf = BoxArray(amrData.boxArray(lev)).coarsen(amrData.RefRatio()[lev]);
          std::vector< std::pair<int,Box> > isects = baf.intersections(bx);               
          for (int ii = 0; ii < isects.size(); ii++){
            Qf[mfi].setVal(0,isects[ii].second,0,nreactions);
            Qr[mfi].setVal(0,isects[ii].second,0,nreactions);
          }
        }

        // Increment volume-weighted sum of each reaction over all levels
        Real vol = 1;
        for (int i=0; i<BL_SPACEDIM; ++i) {
          vol *= amrData.ProbSize()[i] / amrData.ProbDomain()[lev].length(i);
        }

        for (int i=0; i<nreactions; ++i) {
          Qfsum[i] += Qf.sum(i) * vol;
          Qrsum[i] += Qr.sum(i) * vol;
        }
      }

      Print() << "Derive finished for level " << lev << std::endl;
    }

    //ParallelDescriptor::ReduceRealSum(Qsum.dataPtr(),nreactions,ParallelDescriptor::IOProcessorNumber());
    ParallelDescriptor::ReduceRealSum(Qfsum.dataPtr(),nreactions,ParallelDescriptor::IOProcessorNumber());
    ParallelDescriptor::ReduceRealSum(Qrsum.dataPtr(),nreactions,ParallelDescriptor::IOProcessorNumber());

    std::string QPDatom="C"; pp.query("QPDatom",QPDatom);
    std::string QPDlabel = plotFileName; pp.query("QPDlabel",QPDlabel);
    std::string QPDfileName = plotFileName + "_QPD.dat"; pp.query("QPDfileName",QPDfileName);
    
    if (ParallelDescriptor::IOProcessor())
    {
      std::ofstream osfr(QPDfileName.c_str());
      osfr << QPDlabel << std::endl;
      for (int i=0; i<spec_names.size(); ++i)
        osfr << spec_names[i] << " ";
      osfr << std::endl;

      auto edges = getEdges(QPDatom,1,1);
      std::cout<<"\n total edges "<<edges.size()<<std::endl;
      bool dump_edges = false;
      pp.query("dump_edges",dump_edges);
      if (dump_edges) {
        for (auto eit : edges) {
          Print() << eit << std::endl;
        }
      }

      std::map<EdgeList::const_iterator,Real,ELIcompare> Qf,Qr;

      Real normVal = 1;
      for (EdgeList::const_iterator it = edges.begin(); it!=edges.end(); ++it)
      {
        const Vector<std::pair<int,Real> > RWL=it->rwl();
        for (int i=0; i<RWL.size(); ++i)
        {
          //Note: Assumes that the edges are in terms of *mapped* reactions
          Qf[it] += Qfsum[ RWL[i].first ]*RWL[i].second;
          Qr[it] += Qrsum[ RWL[i].first ]*RWL[i].second;
        }
        if (it->touchesSp("CH4") && it->touchesSp("CH3"))
        {
          normVal = 1./(Qf[it]-Qr[it]); // Normalize to CH4 destruction on CH4->CH3 edge
          if (it->right()=="CH4")
            normVal *= -1;
        }
      }
      if (pp.countval("scaleNorm") > 0) {
        Real scaleNorm;
        pp.get("scaleNorm",scaleNorm);
        normVal *= scaleNorm;
      }
      std::cout << "NormVal: " << normVal << std::endl;
      

      for (EdgeList::const_iterator it = edges.begin(); it!=edges.end(); ++it)
      {
        if (normVal!=0)
        {
          Qf[it] *= normVal;
          Qr[it] *= normVal;
        }
        osfr << it->left() << " " << it->right() << " " << Qf[it] << " " << -Qr[it] << '\n'; 
      }

      std::string fuelSpec;
      bool do_dump = false;
      if (pp.countval("fuelSpec") > 0) {
        do_dump = true;
        pp.get("fuelSpec",fuelSpec);
      }
      
      // Dump a subset of the edges to the screen
      if (do_dump)
      {
        for (EdgeList::const_iterator it = edges.begin(); it!=edges.end(); ++it)
        {
          if (it->touchesSp(fuelSpec))
          {
            std::cout << *it << std::endl;
            std::map<std::string,Real> edgeContrib;
            int rSgn = ( it->left()==fuelSpec  ?  -1  :  +1);
            const Vector<std::pair<int,Real> > RWL=it->rwl();
            for (int i=0; i<RWL.size(); ++i) {
              int rxn = RWL[i].first;
              Real wgt = RWL[i].second;
              auto specCoefs = specCoeffsInReactions(rxn);

              // Find name of reaction partner(s), NP = no partner
              int thisSgn;
              for (int j=0; j<specCoefs.size(); ++j)
              {
                if (specCoefs[j].first == fuelSpec)
                  thisSgn = specCoefs[j].second;
              }
              std::string partnerName = "";
              for (int j=0; j<specCoefs.size(); ++j)
              {
                const std::string& sp = specCoefs[j].first;
                if (sp != fuelSpec  &&  thisSgn*specCoefs[j].second > 0)
                  partnerName = ( partnerName != ""  ?  partnerName + "+" + sp : sp);
              }
              if (partnerName=="")
                partnerName="NP";

              edgeContrib[partnerName] += wgt*((Qrsum[rxn])-Qrsum[rxn])*normVal;
            }

            Real sump=0, sumn=0;
            for (std::map<std::string,Real>::const_iterator cit=edgeContrib.begin(); cit!=edgeContrib.end(); ++cit)
            {
              std::cout << "   partner: " << cit->first << " " << cit->second << std::endl;
              if (cit->second > 0.)
                sump += cit->second;
              else
                sumn += cit->second;
            }
            std::cout << "     sum +ve,-ve: " << sump << " " << sumn << std::endl;
          }
        }
      }
    }
  }
  Finalize();
  return 0;
}
