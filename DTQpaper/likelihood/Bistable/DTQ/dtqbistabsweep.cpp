#include <iostream>
#include <armadillo>
#include <cmath>
#include <cassert>
#include <nlopt.hpp>

// Gaussian functions
double gausspdfscalar(double x, double mu, double sigma)
{
  double u = (x - mu) / fabs(sigma);
  double p = (1 / (sqrt (2 * M_PI) * fabs (sigma))) * exp (-(u * u) / 2);
  return p; 
}

arma::vec gausspdf(const arma::vec &x, double mu, double sigma)
{
  arma::vec u = (x - mu) / fabs(sigma);
  arma::vec p = (1 / (sqrt (2 * M_PI) * fabs (sigma))) * arma::exp (-(u % u) / 2);
  return p; 
}

arma::vec gausspdf(double x, const arma::vec &mu, const arma::vec &sigma)
{
  arma::vec u = (x - mu) / arma::abs(sigma);
  arma::vec p = (1 / (sqrt (2 * M_PI) * arma::abs (sigma))) % arma::exp (-(u % u) / 2);
  return p; 
}

// the following typedef is intended to be used for user-defined
// drift and diffusion functions; note that the function depends both
// on a spatial location x and the parameter vector theta
typedef double (*funcPtr)(const double& x, const arma::vec& theta);

// same as above except that the function returns a vector; this is
// suitable for gradients of f and g
typedef arma::vec (*gradPtr)(const double& x, const arma::vec& theta);

class dtq {
  // private data
  int spi; // DTQ steps per interval
  double myh; // DTQ time step
  double myk; // DTQ grid spacing
  int bigm; // DTQ grid goes from -bigm*myk to bigm*myk
  funcPtr f; // DTQ drift function 
  funcPtr g; // DTQ diffusion function

  double deltat; // time step of the data
  arma::vec curtheta; // current value of theta

  gradPtr gradf; // gradient of DTQ drift function w.r.t. theta
  gradPtr gradg; // gradient of DTQ diffusion function w.r.t. theta

  // flags so that we know whether something exists yet
  bool haveMyh = false;
  bool haveProp = false;
  bool haveData = false;
  bool haveGradfg = false;
  bool haveLoglik = false;
  bool haveGradloglik = false;

  // for next two, we use pointers to save memory
  arma::vec *tvec; // vector of times at which data was taken
                   // think of this as a column vector
  arma::mat *odata; // observations at the times given by tvec
                    // think of this as several columns, one per time series
                    // odata->n_rows should equal tvec->n_elem
  int ltvec;
  int numts;

  arma::sp_mat prop; // the propagator matrix (sparse)
  int ylen;
  arma::vec yvec;
  arma::vec fy;
  arma::vec gy;

  arma::mat loglikmat; // log likelihood at each consecutive pair of times
  double loglik; // total log likelihood
  arma::vec gradloglik; // gradient of total log likelihood w.r.t. theta

  // internal helper functions
  int gradFGyvec(arma::mat &, arma::mat &);
  int gradFGdata(arma::cube &, arma::cube &);
  int phatinitgrad(arma::mat &, arma::cube &, const arma::cube &, const arma::cube &);

  public:
    // constructors
    // minimal constructor for likelihood calculation
    dtq(int inspi, double ink, int inm, funcPtr inf, funcPtr ing, arma::vec &theta) : 
      spi(inspi), myk(ink), bigm(inm), f(inf), g(ing), curtheta(theta) {}

    // minimal constructor needed to solve one forward problem
    dtq(double inmyh, double ink, int inm, funcPtr inf, funcPtr ing, arma::vec &theta) : 
      myh(inmyh), myk(ink), bigm(inm), f(inf), g(ing), curtheta(theta) { haveMyh = true; }

    // retrieve mandatory bits of data
    double getH(void) { return myh; }
    double getK(void) { return myk; }
    int getBigm(void) { return bigm; }

    // retrieve other bits of data
    int getLtvec(void);
    int getNumts(void);

    // setting gradients
    int setGrads(gradPtr, gradPtr);

    // compute the propagator
    int compProp(void);

    // setting/loading data
    int setData(arma::vec *, arma::mat *);

    // compute only the log likelihood
    int compLL(void);

    // compute log likelihood and gradient
    int compGrad(void);

    // retrieve log likelihood and gradient
    double getLL();
    int getGrad(arma::vec &);
    int getGrad(std::vector<double> &);

    // reset theta
    int resetTheta(const arma::vec &);
    int resetTheta(const std::vector<double> &);
};

int dtq::setGrads(gradPtr ingradf, gradPtr ingradg)
{
  gradf = ingradf;
  gradg = ingradg; 
  haveGradfg = true;
  return 0;
}

int dtq::resetTheta(const arma::vec &newtheta)
{
  // make sure the new theta is of the right size
  if (newtheta.n_elem != curtheta.n_elem) return 1;
  else
  {
    for (int i=0; i<curtheta.n_elem; i++)
      curtheta[i] = newtheta[i];

    // since you changed theta, you have to recompute some things...
    haveProp = false;
    haveLoglik = false;
    haveGradloglik = false;
    return 0;
  }
}

int dtq::resetTheta(const std::vector<double> &newtheta)
{
  // make sure the new theta is of the right size
  if (newtheta.size() != curtheta.n_elem) return 1;
  else
  {
    for (int i=0; i<curtheta.n_elem; i++)
      curtheta[i] = newtheta[i];

    // since you changed theta, you have to recompute some things...
    haveProp = false;
    haveLoglik = false;
    haveGradloglik = false;
    return 0;
  }
}

int dtq::compProp(void)
{
  // need myh to proceed
  if (! haveMyh) return 1;

  // fundamental size
  ylen = 2*bigm+1;

  // start computing the propagator!
  prop = arma::sp_mat(ylen,ylen);

  // create yvec
  yvec = arma::zeros(ylen);
  for (int i=-bigm; i<=bigm; i++)
    yvec(bigm+i) = i*myk;

  // apply f and g to yvec
  fy = arma::zeros(ylen);
  gy = arma::zeros(ylen);
  for (int i=-bigm; i<=bigm; i++)
  {
    fy(bigm+i) = (*f)(yvec(bigm+i),curtheta);
    gy(bigm+i) = (*g)(yvec(bigm+i),curtheta);
  }

  // normalization "constant"
  arma::vec c0mod = 1.0/(sqrt(2.0*(arma::datum::pi)*myh)*gy);

  // variance
  arma::vec gy2 = gy % gy;
  arma::vec myvar = gy2*myh;

  // compute and set main diagonal
  // prop.diag() = maindiag;
  arma::vec propvals = arma::exp(-(myh/2.0)*(fy%fy)/gy2) % c0mod;
  arma::umat proploc(2, ylen);
  proploc.row(0) = arma::regspace<arma::urowvec>(0, (ylen-1));
  proploc.row(1) = arma::regspace<arma::urowvec>(0, (ylen-1));

  // superdiagonals
  bool done = false;
  int curdiag = 1;
  double mytol = 2.0e-16;
  double refsum = arma::sum(arma::abs(propvals))*myk;
  while (! done)
  {
    arma::vec mymean = curdiag*myk + fy*myh;
    arma::vec thisdiag = arma::exp(-mymean%mymean/(2.0*myvar))%c0mod;
    thisdiag = thisdiag.tail(ylen - curdiag);
    double thissum = arma::sum(arma::abs(thisdiag));
    if ((curdiag == 1) || (thissum > mytol*refsum))
    {
      // prop.diag(curdiag) = thisdiag;
      arma::umat newloc(2, ylen-curdiag);
      newloc.row(0) = arma::regspace<arma::urowvec>(0, (ylen-curdiag-1));
      newloc.row(1) = newloc.row(0) + curdiag;
      proploc = join_horiz(proploc, newloc);
      propvals = join_vert(propvals, thisdiag);
      curdiag++;
      if (curdiag == ylen) done = true;
    }
    else done = true;
  }
  int maxdiag = curdiag;
  for (curdiag=1; curdiag<=maxdiag; curdiag++)
  {
    arma::vec mymean = -curdiag*myk + fy*myh;
    arma::vec thisdiag = arma::exp(-mymean%mymean/(2.0*myvar))%c0mod;
    thisdiag = thisdiag.head(ylen - curdiag);
    // prop.diag(-curdiag) = thisdiag;
    arma::umat newloc(2, ylen-curdiag);
    newloc.row(1) = arma::regspace<arma::urowvec>(0, (ylen-curdiag-1));
    newloc.row(0) = newloc.row(1) + curdiag;
    proploc = join_horiz(proploc, newloc);
    propvals = join_vert(propvals, thisdiag);
  }
  prop = arma::sp_mat(proploc, propvals, ylen, ylen);
  // check normalization, should get all 1's
  // std::cout << myk*sum(prop,0) << '\n';
  haveProp = true;
  return 0;
}

int dtq::setData(arma::vec *intvec, arma::mat *inodata)
{
  int lintvec = (int) intvec->n_elem;
  int inodatac = (int) inodata->n_cols;
  int inodatar = (int) inodata->n_rows;
  // check whether we have at least two points in time,
  // whether number of rows of data equals number of points in time,
  // and whether we have at least one sample path
  if ((lintvec >= 2) && (lintvec==inodatar) && (inodatac>=1))
  {
    tvec = intvec;
    odata = inodata;
    deltat = (*tvec)(1) - (*tvec)(0); // assume equispaced data
    if (haveMyh)
    {
      spi = ceil(deltat/myh);      // or set # of DTQ steps per interval
    }
    else
    {
      myh = deltat/spi;   // set DTQ time step
      haveMyh = true;
    }
    ltvec = lintvec;
    numts = inodatac;
    haveData = true;
    return 0;
  }
  else
  {
    tvec = NULL;
    odata = NULL;
    haveData = false;
    return 1;
  }
}

int dtq::getLtvec(void)
{
  if (haveData)
    return ltvec;
  else
    return 1;
}

int dtq::getNumts(void)
{
  if (haveData)
    return numts;
  else
    return 1;
}


int dtq::compLL(void)
{
  // remember, everything here is for equispaced data
  // we'll save the non-equispaced case for our scala + spark code :)
  if ((! haveData) || (! haveMyh)) return 1;
  if (spi<1) return 1;

  loglikmat = arma::zeros(ltvec-1,numts);

  if (spi==1) // special case
  {
    for (int i=0; i<(ltvec-1); i++)
    {
      for (int j=0; j<numts; j++)
      {
        double xi = (*odata)(i,j);
        double xip1 = (*odata)(i+1,j);
        double mu = xi + ((*f)(xi,curtheta))*myh;
        double myh12 = sqrt(myh);
        double sig = ((*g)(xi,curtheta))*myh12;
        loglikmat(i,j) = log(gausspdfscalar(xip1,mu,sig));
      }
    }
  }
  else
  {
    // build the big matrix of initial conditions
    arma::mat dtqmat = arma::zeros(ylen,(ltvec-1));
    double myh12 = sqrt(myh);
    for (int i=0; i<(ltvec-1); i++)
    {
      // go through each particular initial condition at this time
      // and make a Gaussian
      for (int j=0; j<numts; j++)
      {
        double xi = (*odata)(i,j);
        double mu = xi + ((*f)(xi,curtheta))*myh;
        double sig = ((*g)(xi,curtheta))*myh12;
        dtqmat.col(i) += gausspdf(yvec,mu,sig);
      }
      dtqmat.col(i) = dtqmat.col(i) / numts;
    }

    // propagate this forward in time by (spi-2) steps
    if (spi >= 3)
      for (int i=1; i<=(spi-2); i++)
        dtqmat = myk * prop * dtqmat;

    // now multiply on the left by the Gamma vectors
    arma::vec muvec = yvec + fy*myh;
    arma::vec sigvec = gy*myh12;
    for (int i=0; i<(ltvec-1); i++)
    {
      for (int j=0; j<numts; j++)
      {
        arma::vec gammavec = myk*gausspdf((*odata)(i+1,j),muvec,sigvec);
        loglikmat(i,j) = log(arma::dot(gammavec,dtqmat.col(i)));
      }
    }
  }
  // std::cout << loglikmat << '\n';
  haveLoglik = true;
  return 0;
}

// retrieve the sum total log likelihood
double dtq::getLL(void)
{
  return arma::accu(loglikmat);
}

// retrieve the gradient of the total log likelihood w.r.t. theta
int dtq::getGrad(arma::vec& outvec)
{
  for (int i=0; i<curtheta.n_elem; i++)
    outvec[i] = gradloglik[i];

  return 0;
}

int dtq::getGrad(std::vector<double>& outvec)
{
  for (int i=0; i<curtheta.n_elem; i++)
    outvec[i] = gradloglik[i];

  return 0;
}

// compute the gradients of f and g at all spatial grid points in yvec
int dtq::gradFGyvec(arma::mat &gfy, arma::mat &ggy)
{
  for (int i=0; i<ylen; i++)
  {
    gfy.row(i) = ((*gradf)(yvec(i),curtheta)).t();
    ggy.row(i) = ((*gradg)(yvec(i),curtheta)).t();
  }
  return 0;
}

// compute the gradients of f and g at all data points
int dtq::gradFGdata(arma::cube &gfd, arma::cube &ggd)
{
  for (int j=0; j<(ltvec-1); j++)
  {
    for (int l=0; l<numts; l++)
    {
      double xi = (*odata)(j,l);
      gfd.slice(l).col(j) = (*gradf)(xi,curtheta);
      ggd.slice(l).col(j) = (*gradg)(xi,curtheta);
    }
  }
  return 0;
}

// build the big matrix of initial conditions
// and the gradients of those initial conditions!
int dtq::phatinitgrad(arma::mat &phatI, arma::cube &phatG, const arma::cube &gfd, const arma::cube &ggd)
{
  double myh12 = sqrt(myh);
  for (int j=0; j<(ltvec-1); j++)
  {
    // go through each particular initial condition at this time
    // and make a Gaussian
    for (int l=0; l<numts; l++)
    {
      double xi = (*odata)(j,l);
      double mu = xi + ((*f)(xi,curtheta))*myh;
      double gval = (*g)(xi,curtheta);
      double sig = gval*myh12;
      arma::vec thisphat = gausspdf(yvec,mu,sig);
      phatI.col(j) += thisphat;
      for (int i=0; i<curtheta.n_elem; i++)
      {
        arma::vec pgtemp = (yvec - mu)*gfd(i,j,l)/(gval*gval);
        pgtemp -= ggd(i,j,l)/gval;
        pgtemp += arma::pow(yvec - mu,2)*ggd(i,j,l)/(myh*gval*gval*gval);
        phatG.slice(i).col(j) += pgtemp % thisphat;
      }
    }
  }
  phatI = phatI / numts;
  phatG = phatG / numts;
  return 0;
}

// compute the log likelihood and its gradient w.r.t. theta
int dtq::compGrad(void)
{
  // remember, everything here is for equispaced data
  // we'll save the non-equispaced case for our scala + spark code :)
  if ((! haveData) || (! haveMyh)) return 1;
  if (spi<1) return 1;

  loglikmat = arma::zeros(ltvec-1,numts);

  if (spi==1) // special case
  {
  } 
  else
  {
    // strategy: precompute and store common elements in Mats and Cubs

    // compute gradf and gradg at all spatial grid points
    arma::mat gradfy = arma::zeros(ylen,curtheta.n_elem);
    arma::mat gradgy = arma::zeros(ylen,curtheta.n_elem);
    this->gradFGyvec(gradfy, gradgy);

    // ompute gradf and gradg at all the data points
    arma::cube gradfdata = arma::zeros(curtheta.n_elem, (ltvec-1), numts);
    arma::cube gradgdata = arma::zeros(curtheta.n_elem, (ltvec-1), numts);
    this->gradFGdata(gradfdata, gradgdata);
    
    // initialize cubes to store all states and adjoints,
    // at all internal time points (spi-1),
    // for each pair of time series points (ltvec-1),
    // and at all spatial grid points (ylen)
    arma::cube dtqcube = arma::zeros(ylen,(ltvec-1),(spi-1));
    arma::cube adjcube = arma::zeros(ylen,(ltvec-1),(spi-1));

    // temporary matrix to store the initial state, phatinit
    arma::mat phatinit = arma::zeros(ylen,(ltvec-1));
    
    // cube to store the gradient of the initial state w.r.t. theta
    arma::cube phatgrad = arma::zeros(ylen,(ltvec-1),curtheta.n_elem);

    // build the big matrix of initial conditions
    // and the gradients of those initial conditions!
    this->phatinitgrad(phatinit, phatgrad, gradfdata, gradgdata);
    dtqcube.slice(0) = phatinit;

    // propagate states forward in time by (spi-2) steps
    if (spi >= 3)
      for (int i=1; i<=(spi-2); i++)
        dtqcube.slice(i) = myk * prop * dtqcube.slice(i-1);

    // now multiply on the left by the Gamma vectors
    const arma::vec muvec = yvec + fy*myh;
    const arma::vec sigvec = gy*sqrt(myh);
    arma::cube allgamma = arma::zeros(ylen,numts,(ltvec-1));
    for (int j=0; j<(ltvec-1); j++)
    {
      for (int l=0; l<numts; l++)
      {
        allgamma.slice(j).col(l) = myk*gausspdf((*odata)(j+1,l),muvec,sigvec);
        loglikmat(j,l) = log(arma::dot(allgamma.slice(j).col(l),dtqcube.slice(spi-2).col(j)));
      }
    }

    // std::cout << loglikmat << '\n';

    // initialize the adjoint calculation
    for (int j=0; j<(ltvec-1); j++)
      for (int l=0; l<numts; l++)
        adjcube.slice(spi-2).col(j) += allgamma.slice(j).col(l) / exp(loglikmat(j,l));

    // propagate adjoints backward in time by (spi-2) steps
    arma::sp_mat transprop = prop.t();
    if (spi >= 3)
      for (int i=(spi-2); i>=1; i--)
        adjcube.slice(i-1) = myk * transprop * adjcube.slice(i);

    // stuff that we need for a bunch of gradients
    gradloglik = arma::zeros(curtheta.n_elem);
    arma::vec gvecm1 = arma::pow(gy,-1);
    arma::vec gvecm2 = arma::pow(gy,-2);
    arma::vec gvecm3 = arma::pow(gy,-3);

    // actual gradient calculation
    // proceed element-wise through theta_i
    for (int i=0; i<curtheta.n_elem; i++)
    {
      arma::vec temp1 = gvecm2 % gradfy.col(i);
      arma::vec temp2 = gvecm1 % gradgy.col(i);
      arma::vec temp3 = (1.0/myh)*gvecm3 % gradgy.col(i);
      arma::sp_mat::const_iterator start = prop.begin();
      arma::sp_mat::const_iterator end = prop.end();
      arma::umat dkdtloc(2, prop.n_nonzero);
      arma::vec dkdtval(prop.n_nonzero);
      unsigned int dkdtc = 0;
      for (arma::sp_mat::const_iterator it = start; it != end; ++it)
      {
        dkdtloc(0,dkdtc) = it.row();
        dkdtloc(1,dkdtc) = it.col();
        dkdtc++;
      }
#pragma omp parallel for
      for (unsigned int dkdtcount=0; dkdtcount < prop.n_nonzero; dkdtcount++)
      {
        unsigned int orow = dkdtloc(0,dkdtcount);
        unsigned int ocol = dkdtloc(1,dkdtcount);
        double comval = yvec(orow) - muvec(ocol);
        dkdtval(dkdtcount) = myk*(prop.values[dkdtcount])*( comval*temp1(ocol) - temp2(ocol) + temp3(ocol)*comval*comval );
      }
      arma::sp_mat dkdtheta(dkdtloc, dkdtval, ylen, ylen, false, true);

      // implement formula (22) from the DSAA paper
      // need gradient of Gamma{F-1}
      double tally = 0.0;
#pragma omp parallel for reduction(+:tally)
      for (int j=0; j<(ltvec-1); j++)
      {
        tally += arma::dot(phatgrad.slice(i).col(j),adjcube.slice(0).col(j));
      }

#pragma omp parallel for collapse(2) reduction(+:tally)
      for (int j=0; j<(ltvec-1); j++)
        for (int l=0; l<numts; l++)
        {
          double xi = (*odata)((j+1),l);
          arma::vec gammagrad = (xi-muvec) % temp1;
          gammagrad += arma::pow(xi-muvec,2) % temp3;
          gammagrad -= temp2;
          gammagrad = gammagrad % allgamma.slice(j).col(l);
          tally += arma::dot(gammagrad,dtqcube.slice(spi-2).col(j)) / exp(loglikmat(j,l));
        }

      // we have tested and found that the dot product is better than the
      // triple matrix product here, i.e., it is worth taking the transpose
      // arma::mat dkdtheta = dkdthetatrans.t();
#pragma omp parallel for collapse(2) reduction(+:tally)
      for (int j=0; j<(ltvec-1); j++)
        for (int l=0; l<(spi-2); l++)
        {
          tally += arma::dot((dkdtheta*dtqcube.slice(l).col(j)),adjcube.slice(l+1).col(j));
        }
      gradloglik(i) = tally;
    }
  }
  haveLoglik = true;
  haveGradloglik = true;
  return 0;
}

double myf(const double& x, const arma::vec& theta)
{
  return theta[0]*x*(theta[1] - x*x);
}

double myg(const double& x, const arma::vec& theta)
{
  return 0.25*sqrt(1.0 + 9.0*x*x);
}

arma::vec myfgrad(const double& x, const arma::vec& theta)
{
  arma::vec outgrad = arma::zeros(2);
  outgrad(0) = x*(theta[1] - x*x);
  outgrad(1) = theta[0]*x;
  // outgrad(2) = 0.0;
  // outgrad(3) = 0.0;
  return outgrad;
}

arma::vec myggrad(const double& x, const arma::vec& theta)
{
  arma::vec outgrad = arma::zeros(2);
  outgrad(0) = 0;
  outgrad(1) = 0;
  // outgrad(2) = sqrt(1.0 + theta[3]*theta[3]*x*x);
  // outgrad(3) = x*x*theta[2]*theta[3]/sqrt(1.0 + theta[3]*theta[3]*x*x);
  return outgrad;
}

dtq *setupDTQ(void)
{
  funcPtr myfptr = &myf;
  funcPtr mygptr = &myg;

  int myspi = 10;
  double myh = 0.1/myspi;
  double myk = pow(myh,0.75);
  int mybigm = ceil(M_PI/pow(myk,1.5)/4.0);

  arma::vec th = {1.0, 1.0}; //, 1.0, 1.0};

  dtq *outdtq;
  outdtq = new dtq(myspi, myk, mybigm, myfptr, mygptr, th);

  gradPtr myfgptr = &myfgrad;  
  gradPtr myggptr = &myggrad;
  int status = outdtq->setGrads( myfgptr, myggptr );
  assert(status==0);

  return outdtq;
}

int main(void)
{
#ifdef _OPENMP
  omp_set_num_threads(48);
#endif

  dtq *mydtq = setupDTQ();

  arma::vec tvec = arma::linspace<arma::vec>(0.1, 20.0, 200);

  // load data from disk!
  arma::mat traj1;
  arma::mat traj2;
  traj1.load("../traj1.txt", arma::csv_ascii);
  traj2.load("../traj2.txt", arma::csv_ascii);
  arma::mat ts = arma::join_rows(traj1.t(), traj2.t());

  // add data to the mydtq object
  int status = mydtq->setData( &tvec, &ts );
  assert(status==0);

  // create a grid in theta[0], theta[1] space
  // for each grid point, evaluate and print the log likelihood
  unsigned int n0 = 50;
  unsigned int n1 = 50;
  arma::vec theta0grid = arma::linspace<arma::vec>(-1.0, 4.0, n0);
  arma::vec theta1grid = arma::linspace<arma::vec>(5.0, 12.5, n1);
  double ll = 0.0;
  arma::vec theta = arma::zeros<arma::vec>(2);
  
  for (int i0=0; i0<n0; i0++)
  {
    for (int i1=0; i1<n1; i1++)
    {
      theta[0] = theta0grid[i0];
      theta[1] = theta1grid[i1];
      int rv = mydtq->resetTheta(theta);
      assert(rv==0);

      rv = mydtq->compProp();
      assert(rv==0);

      rv = mydtq->compLL();
      assert(rv==0);

      ll = mydtq->getLL();
      std::cout << theta[0] << ", " << theta[1] << ", " <<  ll << '\n';
    }
  }

  delete mydtq;
  return 0;
}

