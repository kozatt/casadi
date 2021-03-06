#
#     This file is part of CasADi.
# 
#     CasADi -- A symbolic framework for dynamic optimization.
#     Copyright (C) 2010 by Joel Andersson, Moritz Diehl, K.U.Leuven. All rights reserved.
# 
#     CasADi is free software; you can redistribute it and/or
#     modify it under the terms of the GNU Lesser General Public
#     License as published by the Free Software Foundation; either
#     version 3 of the License, or (at your option) any later version.
# 
#     CasADi is distributed in the hope that it will be useful,
#     but WITHOUT ANY WARRANTY; without even the implied warranty of
#     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
#     Lesser General Public License for more details.
# 
#     You should have received a copy of the GNU Lesser General Public
#     License along with CasADi; if not, write to the Free Software
#     Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
# 
# 
from casadi import *
from casadi.tools import *

x = SX("x")
y = SX("y")

dotsave(x**8,filename='SX0.pdf')

z = jacobian(sqrt(x**2+y**2),x)
print "done"

dotsave(z,filename='SX1.pdf')

z = jacobian(sqrt(x**2+y**2),[x,y])

dotsave(z,filename='SX2.pdf')

z = SXMatrix(4,5)
z[:,1] = x
z[1,:] = x**2+y**2
z[3,3] = 66.6
z[3,2] = cos(y)

dotsave(z,filename='SX3.pdf')

