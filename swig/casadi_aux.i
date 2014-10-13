/*
 *    This file is part of CasADi.
 *
 *    CasADi -- A symbolic framework for dynamic optimization.
 *    Copyright (C) 2010-2014 Joel Andersson, Joris Gillis, Moritz Diehl,
 *                            K.U. Leuven. All rights reserved.
 *    Copyright (C) 2011-2014 Greg Horn
 *
 *    CasADi is free software; you can redistribute it and/or
 *    modify it under the terms of the GNU Lesser General Public
 *    License as published by the Free Software Foundation; either
 *    version 3 of the License, or (at your option) any later version.
 *
 *    CasADi is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *    Lesser General Public License for more details.
 *
 *    You should have received a copy of the GNU Lesser General Public
 *    License along with CasADi; if not, write to the Free Software
 *    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */


%{
#include <sstream>
#include "casadi/core/std_vector_tools.hpp"
#include "casadi/core/printable_object.hpp"
#include "casadi/core/shared_object.hpp"
#include "casadi/core/weak_ref.hpp"
#include "casadi/core/generic_type.hpp"
#include "casadi/core/options_functionality.hpp"
#include "casadi/core/casadi_calculus.hpp"
%}

%include "casadi/core/std_vector_tools.hpp"
VECTOR_TOOLS_TEMPLATES(int)
VECTOR_TOOLS_TEMPLATES(double)

%define VECTOR_REPR(type)
%extend std::vector< type >{
  std::string SWIG_REPR(){ return casadi::getRepresentation(*$self); }
  std::string SWIG_STR(){ return casadi::getDescription(*$self); }
};
%enddef

#ifdef SWIGPYTHON
%rename(SWIG_STR) getDescription;
#endif // SWIGPYTHON
%include "casadi/core/printable_object.hpp"

%template(PrintSharedObject)           casadi::PrintableObject<casadi::SharedObject>;
%include "casadi/core/shared_object.hpp"
%include "casadi/core/weak_ref.hpp"
%include "casadi/core/casadi_types.hpp"
%include "casadi/core/generic_type.hpp"
%include "casadi/core/options_functionality.hpp"
%include "casadi/core/casadi_calculus.hpp"

namespace casadi {
  %extend OptionsFunctionality {
    void setOption(const std::string &name, const std::string& val){$self->setOption(name,val);} 
    void setOption(const std::string &name, const std::vector<int>& val){$self->setOption(name,val);} 
    void setOption(const std::string &name, const std::vector<double>& val){$self->setOption(name,val);} 
    void setOption(const std::string &name, double val){$self->setOption(name,val);}
    void setOption(const std::string &name, int val){$self->setOption(name,val);} 
    void setOption(const std::string &name, bool val){$self->setOption(name,val);}  
  }
} // namespace casadi

