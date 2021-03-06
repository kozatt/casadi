/*
 *    This file is part of CasADi.
 *
 *    CasADi -- A symbolic framework for dynamic optimization.
 *    Copyright (C) 2010 by Joel Andersson, Moritz Diehl, K.U.Leuven. All rights reserved.
 *
 *    CasADi is free software; you can redistribute it and/or
 *    modify it under the terms of the GNU Lesser General Public
 *    License as published by the Free Software Foundation; either
 *    version 3 of the License, or (at your option) any later version.
 *
 *    CasADi is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSefcn.  See the GNU
 *    Lesser General Public License for more details.
 *
 *    You should have received a copy of the GNU Lesser General Public
 *    License along with CasADi; if not, write to the Free Software
 *    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <symbolic/casadi.hpp>
#include <interfaces/qpoases/qpoases_solver.hpp>
#include <interfaces/ipopt/ipopt_solver.hpp>
#include <nonlinear_programming/nlp_qp_solver.hpp>
#include <nonlinear_programming/scpgen.hpp>

#include <iomanip>
#include <ctime>
#include <cstdlib>

using namespace CasADi;
using namespace std;

class Tester{
public:
  // Constructor
  Tester(int n_boxes, int n_euler, int n_finite_elements, int n_meas) : n_boxes_(n_boxes), n_euler_(n_euler), n_finite_elements_(n_finite_elements), n_meas_(n_meas){}

  // Perform the modelling
  void model();

  // Simulate to generae measurements
  void simulate(double drag_true, double depth_true);

  // Transscribe as an NLP
  void transcribe(bool single_shooting, bool gauss_newton, bool codegen, bool ipopt_as_qp_solver, bool regularize, double reg_threshold);
  
  // Solve the NLP
  void optimize(double drag_guess, double depth_guess, int& iter_count, double& sol_time, double& drag_est, double& depth_est);

  // Dimensions
  int n_boxes_;
  int n_euler_;
  int n_finite_elements_;
  int n_meas_;

  // Initial conditions
  DMatrix u0_;
  DMatrix v0_;
  DMatrix h0_;

  // Discrete time dynamics
  FX f_;
  
  // Generated measurements
  vector<DMatrix> H_meas_;

  // Height of the splash
  double spheight_;

  // Scaling factors for the parameters
  vector<double> p_scale_;

  /// NLP solver
  NLPSolver nlp_solver_;
};

void Tester::model(){
  // Physical parameters
  double g = 9.81; // gravity
  double poolwidth = 0.2;
  double sprad = 0.03;
  spheight_ = 0.01;
  double endtime = 1.0;
    
  // Discretization
  int ntimesteps = n_euler_*n_meas_;
  double dt = endtime/ntimesteps;
  double dx = poolwidth/n_boxes_;
  double dy = poolwidth/n_boxes_;
  vector<double> x(n_boxes_), y(n_boxes_);
  for(int i=0; i<n_boxes_; ++i){
    x[i] = (i+0.5)*dx;
    y[i] = (i+0.5)*dy;
  }

  // Initial conditions
  u0_ = DMatrix::zeros(n_boxes_+1,n_boxes_  );
  v0_ = DMatrix::zeros(n_boxes_  ,n_boxes_+1);
  h0_ = DMatrix::zeros(n_boxes_  ,n_boxes_  );
  bool any_point_in_domain = false;
  for(int i=0; i<n_boxes_; ++i){
    for(int j=0; j<n_boxes_; ++j){
      double spdist = sqrt(pow((x[i]-0.04),2.) + pow((y[j]-0.04),2.));
      if(spdist<sprad/3.0){
	h0_.elem(i,j) = spheight_ * cos(3.0*M_PI*spdist/(2.0*sprad));
	any_point_in_domain = true;
      }
    }
  }

  // Make sure that there is at least one point with nonzero initial values
  if(!any_point_in_domain){
    int i_splash = std::min(int(0.04/dx),n_boxes_-1);
    int j_splash = std::min(int(0.04/dy),n_boxes_-1);
    h0_.elem(i_splash,j_splash) = spheight_;
  }
  
  // Free parameters (nominal values)
  SX drag_nom("b");
  SX depth_nom("H");
  vector<SX> p(2); p[0]=drag_nom; p[1]=depth_nom;
  
  // Scaling factors for the parameters
  double drag_scale = 1;
  double depth_scale = 0.01;
  p_scale_.resize(2);
  p_scale_[0] = drag_scale;
  p_scale_[1] = depth_scale;

  // Real parameter values
  SX drag = drag_nom*drag_scale;
  SX depth = depth_nom*depth_scale;

  // The state at a measurement
  SXMatrix uk = ssym("uk",n_boxes_+1, n_boxes_);
  SXMatrix vk = ssym("vk",n_boxes_  , n_boxes_+1);
  SXMatrix hk = ssym("hk",n_boxes_  , n_boxes_);
  
  // Take one step of the integrator
  SXMatrix u = uk;
  SXMatrix v = vk;
  SXMatrix h = hk;
  
  // Temporaries
  SX d1 = -dt*g/dx;
  SX d2 = dt*drag;
  
  // Update u
  for(int i=0; i<n_boxes_-1; ++i){
    for(int j=0; j<n_boxes_; ++j){
      u.elem(1+i,j) += d1*(h.elem(1+i,j)-h.elem(i,j))- d2*u.elem(1+i,j);
    }
  }
  
  // Update v
  d1 = -dt*g/dy;
  for(int i=0; i<n_boxes_; ++i){
    for(int j=0; j<n_boxes_-1; ++j){
      v.elem(i,j+1) += d1*(h.elem(i,j+1)-h.elem(i,j))- d2*v.elem(i,j+1);
    }
  }
  
  // Update h
  d1 = (-depth*dt)*(1.0/dx);
  d2 = (-depth*dt)*(1.0/dy);
  for(int i=0; i<n_boxes_; ++i){
    for(int j=0; j<n_boxes_; ++j){
      h.elem(i,j) += d1*(u.elem(1+i,j)-u.elem(i,j)) + d2*(v.elem(i,j+1)-v.elem(i,j));
    }
  }
  
  // Create an integrator function
  vector<SXMatrix> f_step_in(4);
  f_step_in[0] = p;
  f_step_in[1] = uk;
  f_step_in[2] = vk;
  f_step_in[3] = hk;
  vector<SXMatrix> f_step_out(3);
  f_step_out[0] = u;
  f_step_out[1] = v;
  f_step_out[2] = h;
  SXFunction f_step(f_step_in,f_step_out);
  f_step.init();
  cout << "generated single step dynamics (" << f_step.getAlgorithmSize() << " nodes)" << endl;
  
  // Integrate over one subinterval
  vector<MX> f_in(4);
  MX P = msym("P",2);
  MX Uk = msym("Uk",n_boxes_+1, n_boxes_);
  MX Vk = msym("Vk",n_boxes_  , n_boxes_+1);
  MX Hk = msym("Hk",n_boxes_  , n_boxes_);
  f_in[0] = P;
  f_in[1] = Uk;
  f_in[2] = Vk;
  f_in[3] = Hk;
  vector<MX> f_inter = f_in;
  vector<MX> f_out;
  for(int j=0; j<n_euler_; ++j){
    // Create a call node
    f_out = f_step.call(f_inter);
    
    // Save intermediate state
    f_inter[1] = f_out[0];
    f_inter[2] = f_out[1];
    f_inter[3] = f_out[2];
  }
  
  // Create an integrator function
  MXFunction f_mx(f_in,f_out);
  f_mx.init();
  cout << "generated discrete dynamics for one finite element (" << f_mx.countNodes() << " MX nodes)" << endl;
  
  // Integrate over the complete interval
  if(n_finite_elements_>1){
    f_in[0] = P;
    f_in[1] = Uk;
    f_in[2] = Vk;
    f_in[3] = Hk;
    f_inter = f_in;
    for(int j=0; j<n_finite_elements_; ++j){
      // Create a call node
      f_out = f_mx.call(f_inter);
      
      // Save intermediate state
      f_inter[1] = f_out[0];
      f_inter[2] = f_out[1];
      f_inter[3] = f_out[2];
    }
    
    // Create an integrator function
    f_mx = MXFunction(f_in,f_out);
    f_mx.init();
    cout << "generated discrete dynamics for complete interval (" << f_mx.countNodes() << " MX nodes)" << endl;    
  }
    
  // Expand the discrete dynamics
  if(false){
    SXFunction f_sx(f_mx);
    f_sx.init();
    cout << "generated discrete dynamics, SX (" << f_sx.getAlgorithmSize() << " nodes)" << endl;
    f_ = f_sx;
  } else {
    f_ = f_mx;
  }
}

void Tester::simulate(double drag_true, double depth_true){
  
  // Measurements
  H_meas_.reserve(n_meas_);
  
  // Unscaled parameter values
  vector<double> p_true(2); p_true[0]=drag_true; p_true[1]=depth_true;
  for(int i=0; i<2; ++i){
    p_true[i] /= p_scale_[i];
  }

  // Simulate once to generate "measurements"
  f_.setInput(p_true,0);
  f_.setInput(u0_,1);
  f_.setInput(v0_,2);
  f_.setInput(h0_,3);
  clock_t time1 = clock();
  for(int k=0; k<n_meas_; ++k){
    f_.evaluate();
    const DMatrix& u = f_.output(0);
    const DMatrix& v = f_.output(1);
    const DMatrix& h = f_.output(2);
    f_.setInput(u,1);
    f_.setInput(v,2);
    f_.setInput(h,3);
    
    // Save a copy of h
    H_meas_.push_back(h);
  }
  clock_t time2 = clock();
  double t_elapsed = double(time2-time1)/CLOCKS_PER_SEC;
  cout << "measurements generated in " << t_elapsed << " seconds." << endl;
}


void Tester::transcribe(bool single_shooting, bool gauss_newton, bool codegen, bool ipopt_as_qp_solver, bool regularize, double reg_threshold){
  
  // NLP variables
  MX P = msym("P",2);

  // Variables in the lifted NLP
  stringstream ss;
  
  // Least-squares objective function
  MX nlp_f;
  
  // Constraint function
  MX nlp_g = MX::sparse(0,1);
  
  // Generate full-space NLP
  MX U = u0_;  MX V = v0_;  MX H = h0_;
  for(int k=0; k<n_meas_; ++k){
    // Take a step
    MX f_arg[4] = {P,U,V,H};
    vector<MX> f_res = f_.call(vector<MX>(f_arg,f_arg+4));
    U = f_res[0];    V = f_res[1];    H = f_res[2];
    
    // Lift the height
    if(!single_shooting){
      // Initialize with measurements
      H.lift(H_meas_[k]);

      // Initialize with initial conditions
      //U.lift(u0_);
      //V.lift(v0_);
      //H.lift(DMatrix::zeros(n_boxes_  ,n_boxes_));
      
      // Initialize through simulation
      //      U.lift(U);
      //      V.lift(V);
      //      H.lift(H);
    }
    
    // Objective function term
    nlp_f.append(H-H_meas_[k]);

    // Add to the constraints
    nlp_g.append(H);
  }

  // Reshape to vectors
  nlp_f = flatten(nlp_f);
  nlp_g = flatten(nlp_g);

  // Form scalar objective function if not Gauss-Newton
  if(!gauss_newton){
    nlp_f = inner_prod(nlp_f,nlp_f)/2;
  }
 
  // Objective function
  MXFunction ffcn(P,nlp_f);

  // Constraints
  MXFunction gfcn(P,nlp_g);

  cout << "Generated single-shooting NLP" << endl;
  
  // NLP Solver
  nlp_solver_ = SCPgen(ffcn,gfcn);

  nlp_solver_.setOption("verbose",true);
  nlp_solver_.setOption("gauss_newton",gauss_newton);
  nlp_solver_.setOption("regularize",regularize);
  nlp_solver_.setOption("codegen",codegen);
  nlp_solver_.setOption("reg_threshold",reg_threshold);
  nlp_solver_.setOption("maxiter_ls",3);
  nlp_solver_.setOption("beta",0.5);
  //nlp_solver_.setOption("merit_memory",1);
  nlp_solver_.setOption("maxiter",100);
  
  // Name the variables
  vector<string> variable_name;
  variable_name.push_back("drag");
  variable_name.push_back("depth");
  nlp_solver_.setOption("name_x",variable_name);

  // Print both of the variables
  nlp_solver_.setOption("print_x",range(2));

  Dictionary qp_solver_options;
  if(ipopt_as_qp_solver){
    nlp_solver_.setOption("qp_solver",NLPQPSolver::creator);
    qp_solver_options["nlp_solver"] = IpoptSolver::creator;
    Dictionary nlp_solver_options;
    nlp_solver_options["tol"] = 1e-12;
    nlp_solver_options["print_level"] = 0;
    nlp_solver_options["print_time"] = false;
    qp_solver_options["nlp_solver_options"] = nlp_solver_options;
    
  } else {
    nlp_solver_.setOption("qp_solver",QPOasesSolver::creator);
    qp_solver_options["printLevel"] = "none";
  }
  nlp_solver_.setOption("qp_solver_options",qp_solver_options);
  nlp_solver_.init();

}

void Tester::optimize(double drag_guess, double depth_guess, int& iter_count, double& sol_time, double& drag_est, double& depth_est){
  cout << "Starting parameter estimation" << endl;
    
  // Initial guess
  vector<double> p_init(2);
  p_init[0] = drag_guess/p_scale_[0];
  p_init[1] = depth_guess/p_scale_[1];
  nlp_solver_.setInput(p_init,NLP_X_INIT);

  // Bounds on the variables
  vector<double> lbu(2), ubu(2);  
  lbu.at(0) = 1.0e-1 / p_scale_[0]; // drag positive
  lbu.at(1) = 5.0e-4 / p_scale_[1]; // depth positive
  nlp_solver_.setInput(lbu,NLP_LBX);

  ubu.at(0) = 50.0 / p_scale_[0]; // max drag
  ubu.at(1) =  0.05 / p_scale_[1]; // max depth
  nlp_solver_.setInput(ubu,NLP_UBX);

  // Constraint bounds
  nlp_solver_.setInput(-spheight_, NLP_LBG);
  nlp_solver_.setInput( spheight_, NLP_UBG);

  clock_t time1 = clock();
  nlp_solver_.solve();
  clock_t time2 = clock();
  
  // Solution statistics  
  sol_time = double(time2-time1)/CLOCKS_PER_SEC;
  const vector<double>& x_opt = nlp_solver_.output(NLP_X_OPT).data();
  drag_est = x_opt.at(0)*p_scale_[0];
  depth_est = x_opt.at(1)*p_scale_[1];
  iter_count = nlp_solver_.getStat("iter_count");
}

int main(){

  // True parameter values
  double drag_true = 2.0, depth_true = 0.01;
  
  // Use IPOPT as QP solver (can handle non-convex QPs)
  bool ipopt_as_qp_solver = false;

  // Use Gauss-Newton method
  bool gauss_newton = true;

  // Codegen the Lifted Newton functions
  bool codegen = false;
  
  // Regularize the QP
  bool regularize = true;

  // Smallest allowed eigenvalue for the regularization
  double reg_threshold = 1e-8;

  // Problem size
  // int  n_boxes = 100, n_euler = 100, n_finite_elements = 1, n_meas = 20;
  int  n_boxes = 30, n_euler = 10, n_finite_elements = 10, n_meas = 100; // Paper
  //  int n_boxes = 15, n_euler = 20, n_finite_elements = 1, n_meas = 20;

  // Initial guesses
  vector<double> drag_guess, depth_guess;
  drag_guess.push_back( 2.0); depth_guess.push_back(0.01); // Optimal solution
  drag_guess.push_back( 0.5); depth_guess.push_back(0.01);
  drag_guess.push_back( 5.0); depth_guess.push_back(0.01);
  drag_guess.push_back(15.0); depth_guess.push_back(0.01);
  drag_guess.push_back(30.0); depth_guess.push_back(0.01);
  drag_guess.push_back( 2.0); depth_guess.push_back(0.005);
  drag_guess.push_back( 2.0); depth_guess.push_back(0.02);
  drag_guess.push_back( 2.0); depth_guess.push_back(0.1);
  drag_guess.push_back( 0.2); depth_guess.push_back(0.001);
  drag_guess.push_back( 1.0); depth_guess.push_back(0.005);
  drag_guess.push_back( 4.0); depth_guess.push_back(0.02);
  drag_guess.push_back( 1.0); depth_guess.push_back(0.02);
  drag_guess.push_back(20.0); depth_guess.push_back(0.001);
  
  // Number of tests
  const int n_tests = drag_guess.size();

  // Number of iterations
  vector<int> iter_count_gn(n_tests,-1);
  vector<int> iter_count_eh(n_tests,-1);
  
  // Solution time
  vector<double> sol_time_gn(n_tests,-1);
  vector<double> sol_time_eh(n_tests,-1);
  
  // Estimated drag and depth
  vector<double> drag_est_gn(n_tests,-1);
  vector<double> depth_est_gn(n_tests,-1);
  vector<double> drag_est_eh(n_tests,-1);
  vector<double> depth_est_eh(n_tests,-1);
  
  // Create a tester object
  Tester t(n_boxes,n_euler,n_finite_elements,n_meas);
    
  // Perform the modelling
  t.model();

  // Optimization parameters
  t.simulate(drag_true, depth_true);
  
  // For both single and multiple shooting
  for(int sol=0; sol<2; ++sol){

    // Transcribe as an NLP
    bool single_shooting = sol==0;
    t.transcribe(single_shooting, gauss_newton, codegen, ipopt_as_qp_solver, regularize, reg_threshold);
  
    // Run tests
    for(int test=0; test<n_tests; ++test){
      // Print progress
      cout << "test " << test << endl;
      try{
	t.optimize(drag_guess[test],depth_guess[test],
		   sol==0 ? iter_count_gn[test] : iter_count_eh[test],
		   sol==0 ? sol_time_gn[test] : sol_time_eh[test],
		   sol==0 ? drag_est_gn[test] : drag_est_eh[test],
		   sol==0 ? depth_est_gn[test] : depth_est_eh[test]);

	// Estimated drag
      } catch(exception& ex){
	cout << "Test " << test << " failed: " << ex.what() << endl;
      }
    }
  }

  // Tolerance 
  double tol=1e-3;
  
  cout << 
  setw(10) << "drag" <<  "  &" <<
  setw(10) << "depth" << "  &" << 
  setw(10) << "iter_ss" << "  &" << 
  setw(10) << "time_ss" << "  &" <<
  setw(10) << "iter_ms" << "  &" << 
  setw(10) << "time_ms" << "  \\\\ \%" <<
  setw(10) << "edrag_ss" << 
  setw(10) << "edepth_ss" <<
  setw(10) << "edrag_ms" << 
  setw(10) << "edepth_ms" << endl;
  for(int test=0; test<n_tests; ++test){
    cout << setw(10) << drag_guess[test] << "  &";
    cout << setw(10) << depth_guess[test] << "  &";
    if(fabs(drag_est_gn[test]-drag_true) + fabs(depth_est_gn[test]-depth_true)<tol){
      cout << setw(10) << iter_count_gn[test] << "  &";
      cout << setw(10) << sol_time_gn[test] << "  &";
    } else {
      cout << setw(10) << "$\\infty$" << "  &";
      cout << setw(10) << "$\\infty$" << "  &";
    }
    if(fabs(drag_est_eh[test]-drag_true) + fabs(depth_est_eh[test]-depth_true)<tol){
      cout << setw(10) << iter_count_eh[test] << "  &";
      cout << setw(10) << sol_time_eh[test] << "  \\\\ \%";
    } else {
      cout << setw(10) << "$\\infty$" << "  &";
      cout << setw(10) << "$\\infty$" << "  \\\\ \%";
    }
    cout << setw(10) << drag_est_gn[test];
    cout << setw(10) << depth_est_gn[test];
    cout << setw(10) << drag_est_eh[test];
    cout << setw(10) << depth_est_eh[test] << endl;
  }
  
  return 0;
}

