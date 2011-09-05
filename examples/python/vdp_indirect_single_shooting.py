from casadi import *
from numpy import *
import matplotlib.pyplot as plt

# time
t = symbolic("t")

# Declare variables (use simple, efficient DAG)
x1=SX("x1"); x2=SX("x2")
x = SXMatrix([x1,x2])

# Control
u = symbolic("u")

# ODE right hand side
xdot = [(1 - x2*x2)*x1 - x2 + u, x1]

# Lagrangian function
L = x1*x1 + x2*x2 + u*u

# Costate
lam = symbolic("lam",2)

# Hamiltonian
H = inner_prod(lam,xdot) + L
Hfcn = SXFunction([x,lam,u,t],[H])
Hfcn.init()

# Costate equations
ldot = -Hfcn.grad(0,0)

## The control must minimize the Hamiltonian, which is:
print "Hamiltonian: ", H

# H is of a convect quadratic form in u: H = u*u + p*u + q, let's get the coefficient p
p = Hfcn.grad(2,0)
p = substitute(p,u,0)

# H's unconstrained minimizer is: u = -p/2
u_opt = -p/2

# We must constrain u to the interval [-0.75, 1.0], convexity of H ensures that the optimum is obtain at the bound when u_opt is outside the interval
u_opt = min(u_opt,1.0)
u_opt = max(u_opt,-0.75)
print "optimal control: ", u_opt

# Augment f with lam_dot and subtitute in the value for the optimal control
f = vertcat((xdot,ldot))
f = substitute(f,u,u_opt)

# Create the right hand side function
rhs_in = DAE_NUM_IN*[[]]
rhs_in[DAE_T] = t
rhs_in[DAE_Y] = vertcat((x,lam))
rhs = SXFunction(rhs_in,[f])

# Create an integrator (CVodes)
I = CVodesIntegrator(rhs)
I.setOption("abstol",1e-8) # abs. tolerance
I.setOption("reltol",1e-8) # rel. tolerance
I.setOption("steps_per_checkpoint",1000)
I.setOption("fsens_err_con",False)
I.setOption("stop_at_end",False)
I.setOption("t0",0.0)
I.setOption("tf",10.0)
I.init()

# The initial state
x_init = array([0.,1.])

# The initial costate
l_init = MX("l_init",2)

# The initial condition for the shooting
X = vertcat((x_init,l_init))

# Call the integrator
X,XP = I.call([X,MX(),MX()])

# Costate at the final time
lam_f = X[2:4]

# Terminal constraints: lam = 0
G = MXFunction([l_init],[lam_f])

# Dummy objective function (there are no degrees of freedom)
F = MXFunction([l_init],[inner_prod(l_init,l_init)])

# Allocate NLP solver
solver = IpoptSolver(F,G)
solver.setOption("hessian_approximation", \
                "limited-memory")
  
# Initialize the NLP solver
solver.init()

# Set bounds and initial guess
solver.setInput([-inf,-inf], NLP_LBX)
solver.setInput([ inf, inf], NLP_UBX)
solver.setInput([   0,   0], NLP_X_INIT)
solver.setInput([   0,   0], NLP_LBG)
solver.setInput([   0,   0], NLP_UBG)

# Solve the problem
solver.solve()

# Retrieve the optimal solution
l_init_opt = array(solver.output(NLP_X_OPT).data())

# Time grid for visualization
tgrid = linspace(0,10,100)

# Output functions
output_fcn = SXFunction(rhs_in,[x1,x2,u_opt])

# Simulator to get optimal state and control trajectories
simulator = Simulator(I, output_fcn, tgrid)
simulator.init()

# Pass initial conditions to the simulator
simulator.setInput(concatenate((x_init,l_init_opt)),INTEGRATOR_X0)

# Simulate to get the trajectories
simulator.evaluate()

# Get optimal control
x_opt = simulator.output(0)
y_opt = simulator.output(1)
u_opt = simulator.output(2)

# Plot the results
plt.figure(1)
plt.clf()
plt.plot(tgrid,x_opt,'--')
plt.plot(tgrid,y_opt,'-')
plt.plot(tgrid,u_opt,'-.')
plt.title("Van der Pol optimization - indirect single shooting")
plt.xlabel('time')
plt.legend(['x trajectory','y trajectory','u trajectory'])
plt.grid()
plt.show()