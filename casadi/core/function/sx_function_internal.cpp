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


#include "sx_function_internal.hpp"
#include <limits>
#include <stack>
#include <deque>
#include <fstream>
#include <sstream>
#include <iomanip>
#include "../std_vector_tools.hpp"
#include "../sx/sx_tools.hpp"
#include "../sx/sx_node.hpp"
#include "../casadi_types.hpp"
#include "../matrix/sparsity_internal.hpp"
#include "../profiling.hpp"
#include "../casadi_options.hpp"

namespace casadi {

  using namespace std;


  SXFunctionInternal::SXFunctionInternal(const vector<SX >& inputv, const vector<SX >& outputv) :
    XFunctionInternal<SXFunction, SXFunctionInternal, SX, SXNode>(inputv, outputv) {
    setOption("name", "unnamed_sx_function");
    addOption("just_in_time_sparsity", OT_BOOLEAN, false,
              "Propagate sparsity patterns using just-in-time "
              "compilation to a CPU or GPU using OpenCL");
    addOption("just_in_time_opencl", OT_BOOLEAN, false,
              "Just-in-time compilation for numeric evaluation using OpenCL (experimental)");

    // Check for duplicate entries among the input expressions
    bool has_duplicates = false;
    for (vector<SX >::iterator it = inputv_.begin(); it != inputv_.end(); ++it) {
      for (vector<SXElement>::iterator itc = it->begin(); itc != it->end(); ++itc) {
        bool is_duplicate = itc->getTemp()!=0;
        if (is_duplicate) {
          cerr << "Duplicate expression: " << *itc << endl;
        }
        has_duplicates = has_duplicates || is_duplicate;
        itc->setTemp(1);
      }
    }

    // Reset temporaries
    for (vector<SX >::iterator it = inputv_.begin(); it != inputv_.end(); ++it) {
      for (vector<SXElement>::iterator itc = it->begin(); itc != it->end(); ++itc) {
        itc->setTemp(0);
      }
    }

    if (has_duplicates) {
      cout << "Input expressions:" << endl;
      for (int iind=0; iind<inputv_.size(); ++iind) {
        cout << iind << ": " << inputv_[iind] << endl;
      }
      casadi_error("The input expressions are not independent (or were not reset properly).");
    }

    casadi_assert(!outputv_.empty()); // NOTE: Remove?

    // Reset OpenCL memory
#ifdef WITH_OPENCL
    kernel_ = 0;
    program_ = 0;
    sp_fwd_kernel_ = 0;
    sp_adj_kernel_ = 0;
    sp_program_ = 0;
#endif // WITH_OPENCL
  }

  SXFunctionInternal::~SXFunctionInternal() {
    // Free OpenCL memory
#ifdef WITH_OPENCL
    freeOpenCL();
    spFreeOpenCL();
#endif // WITH_OPENCL
  }

  void SXFunctionInternal::evaluate() {
    double time_start=0;
    double time_stop=0;
    if (CasadiOptions::profiling) {
      time_start = getRealTime();
      if (CasadiOptions::profilingBinary) {
        profileWriteEntry(CasadiOptions::profilingLog, this);
      } else {
      CasadiOptions::profilingLog  << "start " << this << ":" <<getOption("name") << std::endl;
      }
    }

    casadi_log("SXFunctionInternal::evaluate():begin  " << getOption("name"));

    // NOTE: The implementation of this function is very delicate. Small changes in the
    // class structure can cause large performance losses. For this reason,
    // the preprocessor macros are used below
    if (!free_vars_.empty()) {
      std::stringstream ss;
      repr(ss);
      casadi_error("Cannot evaluate \"" << ss.str() << "\" since variables "
                   << free_vars_ << " are free.");
    }

#ifdef WITH_OPENCL
    if (just_in_time_opencl_) {
      // Evaluate with OpenCL
      evaluateOpenCL();
      return; // Quick return
    }
#endif // WITH_OPENCL

    // Evaluate the algorithm
    for (vector<AlgEl>::iterator it=algorithm_.begin(); it!=algorithm_.end(); ++it) {
      switch (it->op) {
        // Start by adding all of the built operations
        CASADI_MATH_FUN_BUILTIN(work_[it->i1], work_[it->i2], work_[it->i0])

        // Constant
        case OP_CONST: work_[it->i0] = it->d; break;

        // Load function input to work vector
        case OP_INPUT: work_[it->i0] = inputNoCheck(it->i1).data()[it->i2]; break;

        // Get function output from work vector
        case OP_OUTPUT: outputNoCheck(it->i0).data()[it->i2] = work_[it->i1]; break;
      }
    }

    casadi_log("SXFunctionInternal::evaluate():end " << getOption("name"));

    if (CasadiOptions::profiling) {
      time_stop = getRealTime();
      if (CasadiOptions::profilingBinary) {
        profileWriteExit(CasadiOptions::profilingLog, this, time_stop-time_start);
      } else {
        CasadiOptions::profilingLog
          << (time_stop-time_start)*1e6 << " ns | "
          << (time_stop-time_start)*1e3 << " ms | "
          << this << ":" <<getOption("name")
          << ":0||SX algorithm size: " << algorithm_.size()
          << std::endl;
      }
    }
  }


  SX SXFunctionInternal::hess(int iind, int oind) {
    casadi_assert_message(output(oind).numel() == 1, "Function must be scalar");
    SX g = grad(iind, oind);
    g.densify();
    if (verbose())  cout << "SXFunctionInternal::hess: calculating gradient done " << endl;

    // Create function
    SXFunction gfcn(inputv_.at(iind), g);
    gfcn.setOption("verbose", getOption("verbose"));
    gfcn.init();

    // Calculate jacobian of gradient
    if (verbose()) {
      cout << "SXFunctionInternal::hess: calculating Jacobian " << endl;
    }
    SX ret = gfcn.jac(0, 0, false, true);
    if (verbose()) {
      cout << "SXFunctionInternal::hess: calculating Jacobian done" << endl;
    }

    // Return jacobian of the gradient
    return ret;
  }

  bool SXFunctionInternal::isSmooth() const {
    assertInit();

    // Go through all nodes and check if any node is non-smooth
    for (vector<AlgEl>::const_iterator it = algorithm_.begin(); it!=algorithm_.end(); ++it) {
      if (!operation_checker<SmoothChecker>(it->op)) {
        return false;
      }
    }
    return true;
  }

  void SXFunctionInternal::print(ostream &stream) const {
    FunctionInternal::print(stream);

    // Quick return if not initialized
    if (!isInit()) {
      stream << "Function not initialized" << endl;
      return;
    }

    // Iterator to free variables
    vector<SXElement>::const_iterator p_it = free_vars_.begin();

    // Normal, interpreted output
    for (vector<AlgEl>::const_iterator it = algorithm_.begin(); it!=algorithm_.end(); ++it) {
      if (it->op==OP_OUTPUT) {
        stream << "output[" << it->i0 << "][" << it->i2 << "] = @" << it->i1;
      } else {
        stream << "@" << it->i0 << " = ";
        if (it->op==OP_INPUT) {
          stream << "input[" << it->i1 << "][" << it->i2 << "]";
        } else {
          if (it->op==OP_CONST) {
            stream << it->d;
          } else if (it->op==OP_PARAMETER) {
            stream << *p_it++;
          } else {
            int ndep = casadi_math<double>::ndeps(it->op);
            casadi_math<double>::printPre(it->op, stream);
            for (int c=0; c<ndep; ++c) {
              if (c==0) {
                stream << "@" << it->i1;
              } else {
                casadi_math<double>::printSep(it->op, stream);
                stream << "@" << it->i2;
              }

            }
            casadi_math<double>::printPost(it->op, stream);
          }
        }
      }
      stream << ";" << endl;
    }
  }

  void SXFunctionInternal::generateDeclarations(std::ostream &stream, const std::string& type,
                                                CodeGenerator& gen) const {

    // Make sure that there are no free variables
    if (!free_vars_.empty()) {
      casadi_error("Code generation is not possible since variables "
                   << free_vars_ << " are free.");
    }

    // Add auxiliaries. TODO: Only add the auxiliaries that are actually used
    gen.addAuxiliary(CodeGenerator::AUX_SQ);
    gen.addAuxiliary(CodeGenerator::AUX_SIGN);
  }

  void SXFunctionInternal::generateBody(std::ostream &stream, const std::string& type,
                                        CodeGenerator& gen) const {

    // Which variables have been declared
    vector<bool> declared(work_.size(), false);

    // Run the algorithm
    for (vector<AlgEl>::const_iterator it = algorithm_.begin(); it!=algorithm_.end(); ++it) {
      // Indent
      stream << "  ";

      if (it->op==OP_OUTPUT) {
        stream << "if (r" << it->i0 << "!=0) r" << it->i0 << "[" << it->i2 << "]=" << "a" << it->i1;
      } else {
        // Declare result if not already declared
        if (!declared[it->i0]) {
          stream << type << " ";
          declared[it->i0]=true;
        }

        // Where to store the result
        stream << "a" << it->i0 << "=";

        // What to store
        if (it->op==OP_CONST) {
          gen.printConstant(stream, it->d);
        } else if (it->op==OP_INPUT) {
          stream << "x" << it->i1 << "[" << it->i2 << "]";
        } else {
          int ndep = casadi_math<double>::ndeps(it->op);
          casadi_math<double>::printPre(it->op, stream);
          for (int c=0; c<ndep; ++c) {
            if (c==0) {
              stream << "a" << it->i1;
            } else {
              casadi_math<double>::printSep(it->op, stream);
              stream << "a" << it->i2;
            }
          }
          casadi_math<double>::printPost(it->op, stream);
        }
      }
      stream  << ";" << endl;
    }
  }

  void SXFunctionInternal::init() {

    // Call the init function of the base class
    XFunctionInternal<SXFunction, SXFunctionInternal, SX, SXNode>::init();

    // Stack used to sort the computational graph
    stack<SXNode*> s;

    // All nodes
    vector<SXNode*> nodes;

    // Add the list of nodes
    int ind=0;
    for (vector<SX >::iterator it = outputv_.begin(); it != outputv_.end(); ++it, ++ind) {
      int nz=0;
      for (vector<SXElement>::iterator itc = it->begin(); itc != it->end(); ++itc, ++nz) {
        // Add outputs to the list
        s.push(itc->get());
        sort_depth_first(s, nodes);

        // A null pointer means an output instruction
        nodes.push_back(static_cast<SXNode*>(0));
      }
    }

    // Make sure that all inputs have been added also // TODO REMOVE THIS
    for (vector<SX >::iterator it = inputv_.begin(); it != inputv_.end(); ++it) {
      for (vector<SXElement>::iterator itc = it->begin(); itc != it->end(); ++itc) {
        if (!itc->getTemp()) {
          nodes.push_back(itc->get());
        }
      }
    }

    // Set the temporary variables to be the corresponding place in the sorted graph
    for (int i=0; i<nodes.size(); ++i) {
      if (nodes[i]) {
        nodes[i]->temp = i;
      }
    }

    // Sort the nodes by type
    constants_.clear();
    operations_.clear();
    for (vector<SXNode*>::iterator it = nodes.begin(); it != nodes.end(); ++it) {
      SXNode* t = *it;
      if (t) {
        if (t->isConstant())
          constants_.push_back(SXElement::create(t));
        else if (!t->isSymbolic())
          operations_.push_back(SXElement::create(t));
      }
    }

    // Use live variables?
    bool live_variables = getOption("live_variables");

    // Input instructions
    vector<pair<int, SXNode*> > symb_loc;

    // Current output and nonzero, start with the first one
    int curr_oind, curr_nz=0;
    for (curr_oind=0; curr_oind<outputv_.size(); ++curr_oind) {
      if (outputv_[curr_oind].size()!=0) {
        break;
      }
    }

    // Count the number of times each node is used
    vector<int> refcount(nodes.size(), 0);

    // Get the sequence of instructions for the virtual machine
    algorithm_.resize(0);
    algorithm_.reserve(nodes.size());
    for (vector<SXNode*>::iterator it=nodes.begin(); it!=nodes.end(); ++it) {
      // Current node
      SXNode* n = *it;

      // New element in the algorithm
      AlgEl ae;

      // Get operation
      ae.op = n==0 ? OP_OUTPUT : n->getOp();

      // Get instruction
      switch (ae.op) {
      case OP_CONST: // constant
        ae.d = n->getValue();
        ae.i0 = n->temp;
        break;
      case OP_PARAMETER: // a parameter or input
        symb_loc.push_back(make_pair(algorithm_.size(), n));
        ae.i0 = n->temp;
        break;
      case OP_OUTPUT: // output instruction
        ae.i0 = curr_oind;
        ae.i1 = outputv_[curr_oind].at(curr_nz)->temp;
        ae.i2 = curr_nz;

        // Go to the next nonzero
        curr_nz++;
        if (curr_nz>=outputv_[curr_oind].size()) {
          curr_nz=0;
          curr_oind++;
          for (; curr_oind<outputv_.size(); ++curr_oind) {
            if (outputv_[curr_oind].size()!=0) {
              break;
            }
          }
        }
        break;
      default:       // Unary or binary operation
        ae.i0 = n->temp;
        ae.i1 = n->dep(0).get()->temp;
        ae.i2 = n->dep(1).get()->temp;
      }

      // Number of dependencies
      int ndeps = casadi_math<double>::ndeps(ae.op);

      // Increase count of dependencies
      for (int c=0; c<ndeps; ++c) {
        refcount.at(c==0 ? ae.i1 : ae.i2)++;
      }
      // Add to algorithm
      algorithm_.push_back(ae);
    }

    // Place in the work vector for each of the nodes in the tree (overwrites the reference counter)
    vector<int> place(nodes.size());

    // Stack with unused elements in the work vector
    stack<int> unused;

    // Work vector size
    int worksize = 0;

    // Find a place in the work vector for the operation
    for (vector<AlgEl>::iterator it=algorithm_.begin(); it!=algorithm_.end(); ++it) {

      // Number of dependencies
      int ndeps = casadi_math<double>::ndeps(it->op);

      // decrease reference count of children
      // reverse order so that the first argument will end up at the top of the stack
      for (int c=ndeps-1; c>=0; --c) {
        int ch_ind = c==0 ? it->i1 : it->i2;
        int remaining = --refcount.at(ch_ind);
        if (remaining==0) unused.push(place[ch_ind]);
      }

      // Find a place to store the variable
      if (it->op!=OP_OUTPUT) {
        if (live_variables && !unused.empty()) {
          // Try to reuse a variable from the stack if possible (last in, first out)
          it->i0 = place[it->i0] = unused.top();
          unused.pop();
        } else {
          // Allocate a new variable
          it->i0 = place[it->i0] = worksize++;
        }
      }

      // Save the location of the children
      for (int c=0; c<ndeps; ++c) {
        if (c==0) {
          it->i1 = place[it->i1];
        } else {
          it->i2 = place[it->i2];
        }
      }

      // If binary, make sure that the second argument is the same as the first one
      // (in order to treat all operations as binary) NOTE: ugly
      if (ndeps==1 && it->op!=OP_OUTPUT) {
        it->i2 = it->i1;
      }
    }

    if (verbose()) {
      if (live_variables) {
        cout << "Using live variables: work array is "
             <<  worksize << " instead of " << nodes.size() << endl;
      } else {
        cout << "Live variables disabled." << endl;
      }
    }

    // Allocate work vectors (symbolic/numeric)
    work_.resize(worksize, numeric_limits<double>::quiet_NaN());
    s_work_.resize(worksize);

    // Reset the temporary variables
    for (int i=0; i<nodes.size(); ++i) {
      if (nodes[i]) {
        nodes[i]->temp = 0;
      }
    }

    // Now mark each input's place in the algorithm
    for (vector<pair<int, SXNode*> >::const_iterator it=symb_loc.begin();
         it!=symb_loc.end(); ++it) {
      it->second->temp = it->first+1;
    }

    // Add input instructions
    for (int ind=0; ind<inputv_.size(); ++ind) {
      int nz=0;
      for (vector<SXElement>::iterator itc = inputv_[ind].begin();
          itc != inputv_[ind].end();
          ++itc, ++nz) {
        int i = itc->getTemp()-1;
        if (i>=0) {
          // Mark as input
          algorithm_[i].op = OP_INPUT;

          // Location of the input
          algorithm_[i].i1 = ind;
          algorithm_[i].i2 = nz;

          // Mark input as read
          itc->setTemp(0);
        }
      }
    }

    // Locate free variables
    free_vars_.clear();
    for (vector<pair<int, SXNode*> >::const_iterator it=symb_loc.begin();
         it!=symb_loc.end(); ++it) {
      if (it->second->temp!=0) {
        // Save to list of free parameters
        free_vars_.push_back(SXElement::create(it->second));

        // Remove marker
        it->second->temp=0;
      }
    }

    // Initialize just-in-time compilation for numeric evaluation using OpenCL
    just_in_time_opencl_ = getOption("just_in_time_opencl");
    if (just_in_time_opencl_) {
#ifdef WITH_OPENCL
      freeOpenCL();
      allocOpenCL();
#else // WITH_OPENCL
      casadi_error("Option \"just_in_time_opencl\" true requires CasADi "
                   "to have been compiled with WITH_OPENCL=ON");
#endif // WITH_OPENCL
    }

    // Initialize just-in-time compilation for sparsity propagation using OpenCL
    just_in_time_sparsity_ = getOption("just_in_time_sparsity");
    if (just_in_time_sparsity_) {
#ifdef WITH_OPENCL
      spFreeOpenCL();
      spAllocOpenCL();
#else // WITH_OPENCL
      casadi_error("Option \"just_in_time_sparsity\" true requires CasADi to "
                   "have been compiled with WITH_OPENCL=ON");
#endif // WITH_OPENCL
    }

    if (CasadiOptions::profiling && CasadiOptions::profilingBinary) {

      profileWriteName(CasadiOptions::profilingLog, this, getOption("name"),
                       ProfilingData_FunctionType_SXFunction, algorithm_.size());
      int alg_counter = 0;

      // Iterator to free variables
      vector<SXElement>::const_iterator p_it = free_vars_.begin();

      std::stringstream stream;
      for (vector<AlgEl>::const_iterator it = algorithm_.begin(); it!=algorithm_.end(); ++it) {
        stream.str("");
        if (it->op==OP_OUTPUT) {
          stream << "output[" << it->i0 << "][" << it->i2 << "] = @" << it->i1;
        } else {
          stream << "@" << it->i0 << " = ";
          if (it->op==OP_INPUT) {
            stream << "input[" << it->i1 << "][" << it->i2 << "]";
          } else {
            if (it->op==OP_CONST) {
              stream << it->d;
            } else if (it->op==OP_PARAMETER) {
              stream << *p_it++;
            } else {
              int ndep = casadi_math<double>::ndeps(it->op);
              casadi_math<double>::printPre(it->op, stream);
              for (int c=0; c<ndep; ++c) {
                if (c==0) {
                  stream << "@" << it->i1;
                } else {
                  casadi_math<double>::printSep(it->op, stream);
                  stream << "@" << it->i2;
                }

              }
              casadi_math<double>::printPost(it->op, stream);
            }
          }
        }
        stream << std::endl;
        profileWriteSourceLine(CasadiOptions::profilingLog, this,
                               alg_counter++, stream.str(), it->op);
      }
    }

    // Print
    if (verbose()) {
      cout << "SXFunctionInternal::init Initialized " << getOption("name") << " ("
           << algorithm_.size() << " elementary operations)" << endl;
    }
  }

  void SXFunctionInternal::evalSXsparse(const vector<SX>& arg1, vector<SX>& res1,
                                  const vector<vector<SX> >& fseed, vector<vector<SX> >& fsens,
                                  const vector<vector<SX> >& aseed, vector<vector<SX> >& asens) {
    if (verbose()) cout << "SXFunctionInternal::evalSXsparse begin" << endl;

    // Check if arguments matches the input expressions, in which case the output is known
    // to be the output expressions
    const int checking_depth = 2;
    bool output_given = true;
    for (int i=0; i<arg1.size() && output_given; ++i) {
      for (int j=0; j<arg1[i].size() && output_given; ++j) {
        if (!arg1[i].at(j).isEqual(inputv_[i].at(j), checking_depth)) {
          output_given = false;
        }
      }
    }

    // Copy output if known
    if (output_given) {
      for (int i=0; i<res1.size(); ++i) {
        copy(outputv_[i].begin(), outputv_[i].end(), res1[i].begin());
      }
    }

    // Use the function arguments if possible to avoid problems involving
    // equivalent but different expressions
    const vector<SX>& arg = output_given ? inputv_ : arg1;
    vector<SX>& res = output_given ? outputv_ : res1;

    // Number of forward seeds
    int nfdir = fsens.size();

    // number of adjoint seeds
    int nadir = aseed.size();

    // Do we need taping?
    bool taping = nfdir>0 || nadir>0;

    // Iterator to the binary operations
    vector<SXElement>::const_iterator b_it=operations_.begin();

    // Iterator to stack of constants
    vector<SXElement>::const_iterator c_it = constants_.begin();

    // Iterator to free variables
    vector<SXElement>::const_iterator p_it = free_vars_.begin();

    // Tape
    vector<TapeEl<SXElement> > s_pdwork;
    vector<TapeEl<SXElement> >::iterator it1;
    if (taping) {
      s_pdwork.resize(operations_.size());
      it1 = s_pdwork.begin();
    }

    // Evaluate algorithm
    if (verbose()) cout << "SXFunctionInternal::evalSXsparse evaluating algorithm forward" << endl;
    for (vector<AlgEl>::const_iterator it = algorithm_.begin(); it!=algorithm_.end(); ++it) {
      switch (it->op) {
      case OP_INPUT:
        s_work_[it->i0] = arg[it->i1].data()[it->i2]; break;
      case OP_OUTPUT:
        res[it->i0].data()[it->i2] = s_work_[it->i1];
        break;
      case OP_CONST:
        s_work_[it->i0] = *c_it++;
        break;
      case OP_PARAMETER:
        s_work_[it->i0] = *p_it++; break;
      default:
        {
          // Evaluate the function to a temporary value
          // (as it might overwrite the children in the work vector)
          SXElement f;
          if (output_given) {
            f = *b_it++;
          } else {
            switch (it->op) {
              CASADI_MATH_FUN_BUILTIN(s_work_[it->i1], s_work_[it->i2], f)
                }

            // If this new expression is identical to the expression used
            // to define the algorithm, then reuse
            const int depth = 2; // NOTE: a higher depth could possibly give more savings
            f.assignIfDuplicate(*b_it++, depth);
          }

          // Get the partial derivatives, if requested
          if (taping) {
            switch (it->op) {
              CASADI_MATH_DER_BUILTIN(s_work_[it->i1], s_work_[it->i2], f, it1++->d)
                }
          }

          // Finally save the function value
          s_work_[it->i0] = f;
        }
      }
    }

    // Quick return if no sensitivities
    if (!taping) return;

    // Calculate forward sensitivities
    if (verbose())
      cout << "SXFunctionInternal::evalSXsparse calculating forward derivatives" << endl;
    for (int dir=0; dir<nfdir; ++dir) {
      vector<TapeEl<SXElement> >::const_iterator it2 = s_pdwork.begin();
      for (vector<AlgEl>::const_iterator it = algorithm_.begin(); it!=algorithm_.end(); ++it) {
        switch (it->op) {
        case OP_INPUT:
          s_work_[it->i0] = fseed[dir][it->i1].data()[it->i2]; break;
        case OP_OUTPUT:
          fsens[dir][it->i0].data()[it->i2] = s_work_[it->i1]; break;
        case OP_CONST:
        case OP_PARAMETER:
          s_work_[it->i0] = 0;
          break;
          CASADI_MATH_BINARY_BUILTIN // Binary operation
            s_work_[it->i0] = it2->d[0] * s_work_[it->i1] + it2->d[1] * s_work_[it->i2];it2++;break;
        default: // Unary operation
          s_work_[it->i0] = it2->d[0] * s_work_[it->i1]; it2++;
        }
      }
    }

    // Calculate adjoint sensitivities
    if (verbose()) cout << "SXFunctionInternal::evalSXsparse calculating adjoint derivatives"
                       << endl;
    if (nadir>0) fill(s_work_.begin(), s_work_.end(), 0);
    for (int dir=0; dir<nadir; ++dir) {
      vector<TapeEl<SXElement> >::const_reverse_iterator it2 = s_pdwork.rbegin();
      for (vector<AlgEl>::const_reverse_iterator it = algorithm_.rbegin();
          it!=algorithm_.rend();
          ++it) {
        SXElement seed;
        switch (it->op) {
        case OP_INPUT:
          asens[dir][it->i1].data()[it->i2] = s_work_[it->i0];
          s_work_[it->i0] = 0;
          break;
        case OP_OUTPUT:
          s_work_[it->i1] += aseed[dir][it->i0].data()[it->i2];
          break;
        case OP_CONST:
        case OP_PARAMETER:
          s_work_[it->i0] = 0;
          break;
          CASADI_MATH_BINARY_BUILTIN // Binary operation
            seed = s_work_[it->i0];
          s_work_[it->i0] = 0;
          s_work_[it->i1] += it2->d[0] * seed;
          s_work_[it->i2] += it2->d[1] * seed;
          it2++;
          break;
        default: // Unary operation
          seed = s_work_[it->i0];
          s_work_[it->i0] = 0;
          s_work_[it->i1] += it2->d[0] * seed;
          it2++;
        }
      }
    }
    if (verbose()) cout << "SXFunctionInternal::evalSXsparse end" << endl;
  }

  SXFunctionInternal* SXFunctionInternal::clone() const {
    return new SXFunctionInternal(*this);
  }


  void SXFunctionInternal::clearSymbolic() {
    inputv_.clear();
    outputv_.clear();
    s_work_.clear();
  }

  void SXFunctionInternal::spInit(bool fwd) {
    // Quick return if just-in-time compilation for
    //  sparsity pattern propagation, no work vector needed
#ifdef WITH_OPENCL
    if (just_in_time_sparsity_) {
      return; // Quick return
    }
#endif // WITH_OPENCL

    // We need a work array containing unsigned long rather than doubles.
    // Since the two datatypes have the same size (64 bits)
    // we can save overhead by reusing the double array
    bvec_t *iwork = get_bvec_t(work_);
    if (!fwd) fill_n(iwork, work_.size(), bvec_t(0));
  }

  void SXFunctionInternal::spEvaluate(bool fwd) {
#ifdef WITH_OPENCL
    if (just_in_time_sparsity_) {
      // Evaluate with OpenCL
      spEvaluateOpenCL(fwd);
      return; // Quick return
    }
#endif // WITH_OPENCL

    // Get work array
    bvec_t *iwork = get_bvec_t(work_);

    if (fwd) {
      // Propagate sparsity forward
      for (vector<AlgEl>::iterator it=algorithm_.begin(); it!=algorithm_.end(); ++it) {
        switch (it->op) {
        case OP_CONST:
        case OP_PARAMETER:
          iwork[it->i0] = bvec_t(0); break;
        case OP_INPUT:
          iwork[it->i0] = reinterpret_cast<bvec_t*>(&inputNoCheck(it->i1).front())[it->i2]; break;
        case OP_OUTPUT:
          reinterpret_cast<bvec_t*>(&outputNoCheck(it->i0).front())[it->i2] = iwork[it->i1]; break;
        default: // Unary or binary operation
          iwork[it->i0] = iwork[it->i1] | iwork[it->i2]; break;
        }
      }

    } else { // Backward propagation

      // Propagate sparsity backward
      for (vector<AlgEl>::reverse_iterator it=algorithm_.rbegin(); it!=algorithm_.rend(); ++it) {
        // Temp seed
        bvec_t seed;

        // Propagate seeds
        switch (it->op) {
        case OP_CONST:
        case OP_PARAMETER:
          iwork[it->i0] = 0;
          break;
        case OP_INPUT:
          reinterpret_cast<bvec_t*>(&inputNoCheck(it->i1).front())[it->i2] = iwork[it->i0];
          iwork[it->i0] = 0;
          break;
        case OP_OUTPUT:
          iwork[it->i1] |= reinterpret_cast<bvec_t*>(&outputNoCheck(it->i0).front())[it->i2];
          break;
        default: // Unary or binary operation
          seed = iwork[it->i0];
          iwork[it->i0] = 0;
          iwork[it->i1] |= seed;
          iwork[it->i2] |= seed;
        }
      }
    }
  }

  Function SXFunctionInternal::getFullJacobian() {
    // Get all the inputs
    SX arg = SX::sparse(1, 0);
    for (vector<SX>::const_iterator i=inputv_.begin(); i!=inputv_.end(); ++i) {
      arg.appendColumns(vec(*i).T());
    }

    // Get all the outputs
    SX res = SX::sparse(1, 0);
    for (vector<SX>::const_iterator i=outputv_.begin(); i!=outputv_.end(); ++i) {
      res.appendColumns(vec(*i).T());
    }

    // Generate an expression for the Jacobian
    SX J = casadi::jacobian(res, arg);

    // Generate a function for the full Jacobian
    vector<SX> ret_res(1, J);
    ret_res.insert(ret_res.end(), outputv_.begin(), outputv_.end());
    SXFunction ret(inputv_, ret_res);
    return ret;
  }


#ifdef WITH_OPENCL

  SparsityPropagationKernel::SparsityPropagationKernel() {
    device_id = 0;
    context = 0;
    command_queue = 0;
    platform_id = 0;
    cl_int ret;

    // Get Platform and Device Info
    ret = clGetPlatformIDs(1, &platform_id, &ret_num_platforms);
    casadi_assert(ret == CL_SUCCESS);
    ret = clGetDeviceIDs(platform_id, CL_DEVICE_TYPE_DEFAULT, 1, &device_id, &ret_num_devices);
    casadi_assert(ret == CL_SUCCESS);

    // Create OpenCL context
    context = clCreateContext(NULL, 1, &device_id, NULL, NULL, &ret);
    casadi_assert(ret == CL_SUCCESS);

    // Create Command Queue
    command_queue = clCreateCommandQueue(context, device_id, 0, &ret);
    casadi_assert(ret == CL_SUCCESS);
  }

  SparsityPropagationKernel::~SparsityPropagationKernel() {
    // Clean up
    cl_int ret;
    ret = clFlush(command_queue);
    ret = clFinish(command_queue);
    ret = clReleaseCommandQueue(command_queue);
    ret = clReleaseContext(context);
  }

  // Memory for the kernel singleton
  SparsityPropagationKernel SXFunctionInternal::sparsity_propagation_kernel_;

  void SXFunctionInternal::spAllocOpenCL() {
    // OpenCL return flag
    cl_int ret;

    // Generate the kernel source code
    stringstream ss;

    const char* fcn_name[2] = {"sp_evaluate_fwd", "sp_evaluate_adj"};
    for (int kernel=0; kernel<2; ++kernel) {
      bool use_fwd = kernel==0;
      ss << "__kernel void " << fcn_name[kernel] << "(";
      bool first=true;
      for (int i=0; i<getNumInputs(); ++i) {
        if (first) first=false;
        else      ss << ", ";
        ss << "__global unsigned long *x" << i;
      }
      for (int i=0; i<getNumOutputs(); ++i) {
        if (first) first=false;
        else      ss << ", ";
        ss << "__global unsigned long *r" << i;
      }
      ss << ") { " << endl;

      if (use_fwd) {
        // Which variables have been declared
        vector<bool> declared(work_.size(), false);

        // Propagate sparsity forward
        for (vector<AlgEl>::iterator it=algorithm_.begin(); it!=algorithm_.end(); ++it) {
          if (it->op==OP_OUTPUT) {
            ss << "if (r" << it->i0 << "!=0) r" << it->i0 << "[" << it->i2 << "]=" << "a" << it->i1;
          } else {
            // Declare result if not already declared
            if (!declared[it->i0]) {
              ss << "ulong ";
              declared[it->i0]=true;
            }

            // Where to store the result
            ss << "a" << it->i0 << "=";

            // What to store
            if (it->op==OP_CONST || it->op==OP_PARAMETER) {
              ss << "0";
            } else if (it->op==OP_INPUT) {
              ss << "x" << it->i1 << "[" << it->i2 << "]";
            } else {
              int ndep = casadi_math<double>::ndeps(it->op);
              for (int c=0; c<ndep; ++c) {
                if (c==0) {
                  ss << "a" << it->i1;
                } else {
                  ss << "|";
                  ss << "a" << it->i2;
                }
              }
            }
          }
          ss  << ";" << endl;
        }

      } else { // Backward propagation
        // Temporary variable
        ss << "ulong t;" << endl;

        // Declare and initialize work vector
        for (int i=0; i<work_.size(); ++i) {
          ss << "ulong a" << i << "=0;"<< endl;
        }

        // Propagate sparsity backward
        for (vector<AlgEl>::reverse_iterator it=algorithm_.rbegin(); it!=algorithm_.rend(); ++it) {
          if (it->op==OP_OUTPUT) {
            ss << "if (r" << it->i0 << "!=0) a" << it->i1
               << "|=r" << it->i0 << "[" << it->i2 << "];" << endl;
          } else {
            if (it->op==OP_INPUT) {
              ss << "x" << it->i1 << "[" << it->i2 << "]=a" << it->i0 << "; ";
              ss << "a" << it->i0 << "=0;" << endl;
            } else if (it->op==OP_CONST || it->op==OP_PARAMETER) {
              ss << "a" << it->i0 << "=0;" << endl;
            } else {
              int ndep = casadi_math<double>::ndeps(it->op);
              ss << "t=a" << it->i0 << "; ";
              ss << "a" << it->i0 << "=0; ";
              ss << "a" << it->i1 << "|=" << "t" << "; ";
              if (ndep>1) {
                ss << "a" << it->i2 << "|=" << "t" << "; ";
              }
              ss << endl;
            }
          }
        }
      }
      ss << "}" << endl << endl;
    }

    // Form c-string
    std::string s = ss.str();
    if (verbose()) {
      cout << "Kernel source code for sparsity propagation:" << endl;
      cout << " ***** " << endl;
      cout << s;
      cout << " ***** " << endl;
    }
    const char* cstr = s.c_str();

    // Parse kernel source code
    sp_program_ = clCreateProgramWithSource(sparsity_propagation_kernel_.context,
                                            1, static_cast<const char **>(&cstr), 0, &ret);
    casadi_assert(ret == CL_SUCCESS);
    casadi_assert(sp_program_ != 0);

    // Build Kernel Program
    compileProgram(sp_program_);

    // Create OpenCL kernel for forward propatation
    sp_fwd_kernel_ = clCreateKernel(sp_program_, fcn_name[0], &ret);
    casadi_assert(ret == CL_SUCCESS);

    // Create OpenCL kernel for backward propatation
    sp_adj_kernel_ = clCreateKernel(sp_program_, fcn_name[1], &ret);
    casadi_assert(ret == CL_SUCCESS);

    // Memory buffer for each of the input arrays
    sp_input_memobj_.resize(getNumInputs(), static_cast<cl_mem>(0));
    for (int i=0; i<sp_input_memobj_.size(); ++i) {
      sp_input_memobj_[i] = clCreateBuffer(sparsity_propagation_kernel_.context,
                                           CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR,
                                           inputNoCheck(i).size() * sizeof(cl_ulong),
                                           reinterpret_cast<void*>(inputNoCheck(i).ptr()), &ret);
      casadi_assert(ret == CL_SUCCESS);
    }

    // Memory buffer for each of the output arrays
    sp_output_memobj_.resize(getNumOutputs(), static_cast<cl_mem>(0));
    for (int i=0; i<sp_output_memobj_.size(); ++i) {
      sp_output_memobj_[i] = clCreateBuffer(sparsity_propagation_kernel_.context,
                                            CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR,
                                            outputNoCheck(i).size() * sizeof(cl_ulong),
                                            reinterpret_cast<void*>(outputNoCheck(i).ptr()), &ret);
      casadi_assert(ret == CL_SUCCESS);
    }
  }

  void SXFunctionInternal::spEvaluateOpenCL(bool fwd) {
    // OpenCL return flag
    cl_int ret;

    // Select a kernel
    cl_kernel kernel = fwd ? sp_fwd_kernel_ : sp_adj_kernel_;

    // Set OpenCL Kernel Parameters
    int kernel_arg = 0;

    // Pass inputs
    for (int i=0; i<getNumInputs(); ++i) {
      ret = clSetKernelArg(kernel, kernel_arg++,
                           sizeof(cl_mem), static_cast<void *>(&sp_input_memobj_[i]));
      casadi_assert(ret == CL_SUCCESS);
    }

    // Pass outputs
    for (int i=0; i<getNumOutputs(); ++i) {
      ret = clSetKernelArg(kernel, kernel_arg++,
                           sizeof(cl_mem), static_cast<void *>(&sp_output_memobj_[i]));
      casadi_assert(ret == CL_SUCCESS);
    }

    // Execute OpenCL Kernel
    executeKernel(kernel);

    // Get inputs
    for (int i=0; i<sp_input_memobj_.size(); ++i) {
      ret = clEnqueueReadBuffer(sparsity_propagation_kernel_.command_queue,
                                sp_input_memobj_[i], CL_TRUE, 0,
                                inputNoCheck(i).size() * sizeof(cl_ulong),
                                reinterpret_cast<void*>(inputNoCheck(i).ptr()), 0, NULL, NULL);
      casadi_assert(ret == CL_SUCCESS);
    }

    // Get outputs
    for (int i=0; i<sp_output_memobj_.size(); ++i) {
      ret = clEnqueueReadBuffer(sparsity_propagation_kernel_.command_queue,
                                sp_output_memobj_[i], CL_TRUE, 0,
                                outputNoCheck(i).size() * sizeof(cl_ulong),
                                reinterpret_cast<void*>(outputNoCheck(i).ptr()), 0, NULL, NULL);
      casadi_assert(ret == CL_SUCCESS);
    }
  }

  void SXFunctionInternal::spFreeOpenCL() {
    // OpenCL return flag
    cl_int ret;

    // Clean up memory for input buffers
    for (vector<cl_mem>::iterator i=sp_input_memobj_.begin(); i!=sp_input_memobj_.end(); ++i) {
      if (*i != 0) {
        ret = clReleaseMemObject(*i);
        casadi_assert_warning(ret == CL_SUCCESS, "Freeing OpenCL memory failed");
      }
    }
    sp_input_memobj_.clear();

    // Clean up memory for output buffers
    for (vector<cl_mem>::iterator i=sp_output_memobj_.begin(); i!=sp_output_memobj_.end(); ++i) {
      if (*i != 0) {
        ret = clReleaseMemObject(*i);
        casadi_assert_warning(ret == CL_SUCCESS, "Freeing OpenCL memory failed");
      }
    }
    sp_output_memobj_.clear();

    // Free opencl forward propagation kernel
    if (sp_fwd_kernel_!=0) {
      ret = clReleaseKernel(sp_fwd_kernel_);
      casadi_assert_warning(ret == CL_SUCCESS, "Freeing OpenCL memory failed");
      sp_fwd_kernel_ = 0;
    }

    // Free opencl backward propagation kernel
    if (sp_adj_kernel_!=0) {
      ret = clReleaseKernel(sp_adj_kernel_);
      casadi_assert_warning(ret == CL_SUCCESS, "Freeing OpenCL memory failed");
      sp_adj_kernel_ = 0;
    }

    // Free opencl program
    if (sp_program_!=0) {
      ret = clReleaseProgram(sp_program_);
      casadi_assert_warning(ret == CL_SUCCESS, "Freeing OpenCL memory failed");
      sp_program_ = 0;
    }
  }

  void SXFunctionInternal::allocOpenCL() {
    // OpenCL return flag
    cl_int ret;

    // Generate the kernel source code
    stringstream ss;

    // Add kernel prefix
    ss << "__kernel ";

    // Generate the function
    CodeGenerator gen;
    generateFunction(ss, "evaluate", "__global const double*", "__global double*", "double", gen);

    // Form c-string
    std::string s = ss.str();
    if (verbose()) {
      cout << "Kernel source code for numerical evaluation:" << endl;
      cout << " ***** " << endl;
      cout << s;
      cout << " ***** " << endl;
    }
    const char* cstr = s.c_str();

    // Parse kernel source code
    program_ = clCreateProgramWithSource(sparsity_propagation_kernel_.context, 1,
                                         static_cast<const char **>(&cstr), 0, &ret);
    casadi_assert(ret == CL_SUCCESS);
    casadi_assert(program_ != 0);

    // Build Kernel Program
    compileProgram(program_);

    // Create OpenCL kernel for forward propatation
    kernel_ = clCreateKernel(program_, "evaluate", &ret);
    casadi_assert(ret == CL_SUCCESS);

    // Memory buffer for each of the input arrays
    input_memobj_.resize(getNumInputs(), static_cast<cl_mem>(0));
    for (int i=0; i<input_memobj_.size(); ++i) {
      input_memobj_[i] = clCreateBuffer(sparsity_propagation_kernel_.context,
                                        CL_MEM_READ_ONLY | CL_MEM_USE_HOST_PTR,
                                        inputNoCheck(i).size() * sizeof(cl_double),
                                        static_cast<void*>(inputNoCheck(i).ptr()), &ret);
      casadi_assert(ret == CL_SUCCESS);
    }

    // Memory buffer for each of the output arrays
    output_memobj_.resize(getNumOutputs(), static_cast<cl_mem>(0));
    for (int i=0; i<output_memobj_.size(); ++i) {
      output_memobj_[i] = clCreateBuffer(sparsity_propagation_kernel_.context,
                                         CL_MEM_WRITE_ONLY | CL_MEM_USE_HOST_PTR,
                                         outputNoCheck(i).size() * sizeof(cl_double),
                                         static_cast<void*>(outputNoCheck(i).ptr()), &ret);
      casadi_assert(ret == CL_SUCCESS);
    }


  }

  void SXFunctionInternal::evaluateOpenCL() {
    // OpenCL return flag
    cl_int ret;

    // Set OpenCL Kernel Parameters
    int kernel_arg = 0;

    // Pass inputs
    for (int i=0; i<getNumInputs(); ++i) {
      ret = clSetKernelArg(kernel_, kernel_arg++,
                           sizeof(cl_mem), static_cast<void *>(&input_memobj_[i]));
      casadi_assert(ret == CL_SUCCESS);
    }

    // Pass outputs
    for (int i=0; i<getNumOutputs(); ++i) {
      ret = clSetKernelArg(kernel_, kernel_arg++, sizeof(cl_mem),
                           static_cast<void *>(&output_memobj_[i]));
      casadi_assert(ret == CL_SUCCESS);
    }

    // Execute OpenCL Kernel
    executeKernel(kernel_);

    // Get outputs
    for (int i=0; i<output_memobj_.size(); ++i) {
      ret = clEnqueueReadBuffer(sparsity_propagation_kernel_.command_queue, output_memobj_[i],
                                CL_TRUE, 0,
                                outputNoCheck(i).size() * sizeof(cl_double),
                                reinterpret_cast<void*>(outputNoCheck(i).ptr()), 0, NULL, NULL);
      casadi_assert(ret == CL_SUCCESS);
    }
  }

  void SXFunctionInternal::freeOpenCL() {
    // OpenCL return flag
    cl_int ret;

    // Clean up memory for input buffers
    for (vector<cl_mem>::iterator i=input_memobj_.begin(); i!=input_memobj_.end(); ++i) {
      if (*i != 0) {
        ret = clReleaseMemObject(*i);
        casadi_assert_warning(ret == CL_SUCCESS, "Freeing OpenCL memory failed");
      }
    }
    input_memobj_.clear();

    // Clean up memory for output buffers
    for (vector<cl_mem>::iterator i=output_memobj_.begin(); i!=output_memobj_.end(); ++i) {
      if (*i != 0) {
        ret = clReleaseMemObject(*i);
        casadi_assert_warning(ret == CL_SUCCESS, "Freeing OpenCL memory failed");
      }
    }
    output_memobj_.clear();

    // Free opencl numerical evaluation kernel
    if (kernel_!=0) {
      ret = clReleaseKernel(kernel_);
      casadi_assert_warning(ret == CL_SUCCESS, "Freeing OpenCL memory failed");
      kernel_ = 0;
    }

    // Free opencl program
    if (program_!=0) {
      ret = clReleaseProgram(program_);
      casadi_assert_warning(ret == CL_SUCCESS, "Freeing OpenCL memory failed");
      program_ = 0;
    }
  }

  void SXFunctionInternal::compileProgram(cl_program program) {
    // OpenCL return flag
    cl_int ret;

    ret = clBuildProgram(program, 1, &sparsity_propagation_kernel_.device_id, NULL, NULL, NULL);
    if (ret!=CL_SUCCESS) {
      const char* msg;
      switch (ret) {
      case CL_INVALID_PROGRAM: msg = "Program is not a valid program object."; break;
      case CL_INVALID_VALUE: msg = "(1) Device_list is NULL and num_devices is greater than zero, "
              "or device_list is not NULL and num_devices is zero. (2) pfn_notify "
              "is NULL but user_data is not NULL."; break;
      case CL_INVALID_DEVICE: msg = "OpenCL devices listed in device_list are not in the "
              "list of devices associated with program"; break;
      case CL_INVALID_BINARY: msg = "Program is created with clCreateWithProgramBinary and "
              "devices listed in device_list do not have a valid program binary loaded."; break;
      case CL_INVALID_BUILD_OPTIONS: msg = "The build options specified by options are invalid. ";
          break;
      case CL_INVALID_OPERATION: msg = "(1) The build of a program executable for any of the "
              "devices listed in device_list by a previous call to clBuildProgram for program "
              "has not completed. (2) There are kernel objects attached to program. "; break;
      case CL_COMPILER_NOT_AVAILABLE: msg = "Program is created with clCreateProgramWithSource "
              "and a compiler is not available i.e. CL_DEVICE_COMPILER_AVAILABLE specified "
              "in table 4.3 is set to CL_FALSE."; break;
      case CL_BUILD_PROGRAM_FAILURE: {
        msg = "There is a failure to build the program executable. This error will be "
            "returned if clBuildProgram does not return until the build has completed. ";

        // Determine the size of the log
        size_t log_size;
        clGetProgramBuildInfo(program, sparsity_propagation_kernel_.device_id,
                              CL_PROGRAM_BUILD_LOG, 0, NULL, &log_size);

        // Allocate memory for the log
        std::vector<char> log;
        log.resize(log_size);

        // Print the log
        clGetProgramBuildInfo(program, sparsity_propagation_kernel_.device_id,
                              CL_PROGRAM_BUILD_LOG, log_size, &(log[0]), NULL);
        cerr << log << endl;
        break;
      }
      case CL_OUT_OF_RESOURCES: msg = "There is a failure to allocate resources required by the "
              "OpenCL implementation on the device."; break;
      case CL_OUT_OF_HOST_MEMORY: msg = "There is a failure to allocate resources required by "
              "the OpenCL implementation on the host."; break;
      default: msg = "Unknown error"; break;
      }

      casadi_error("clBuildProgram failed: " << msg);
    }
  }

  void SXFunctionInternal::executeKernel(cl_kernel kernel) {
    // OpenCL return flag
    cl_int ret;

    // Execute OpenCL kernel
    ret = clEnqueueTask(sparsity_propagation_kernel_.command_queue, kernel, 0, NULL, NULL);
    if (ret!=CL_SUCCESS) {
      const char* msg;
      switch (ret) {
      case CL_INVALID_PROGRAM_EXECUTABLE:
          msg = "There is no successfully built program executable available "
                "for device associated with command_queue.";
          break;
      case CL_INVALID_COMMAND_QUEUE: msg = "Command_queue is not a valid command-queue."; break;
      case CL_INVALID_KERNEL: msg = "Kernel is not a valid kernel object."; break;
      case CL_INVALID_CONTEXT:
          msg = "Context associated with command_queue and kernel are not the "
                "same or if the context associated with command_queue and "
                "events in event_wait_list are not the same.";
          break;
      case CL_INVALID_KERNEL_ARGS: msg = "The kernel argument values have not been specified.";
          break;
      case CL_INVALID_WORK_GROUP_SIZE:
          msg = "A work-group size is specified for kernel using the "
              "__attribute__((reqd_work_group_size(X, Y, Z))) qualifier in "
              "program source and is not (1, 1, 1).";
          break;
      case CL_MISALIGNED_SUB_BUFFER_OFFSET:
          msg = "A sub-buffer object is specified as the value for an argument"
              " that is a buffer object and the offset specified when the "
              "sub-buffer object is created is not aligned to "
              "CL_DEVICE_MEM_BASE_ADDR_ALIGN value for device associated with "
              "queue.";
          break;
      case CL_INVALID_IMAGE_SIZE:
          msg = "n image object is specified as an argument value and the image"
              " dimensions (image width, height, specified or compute col "
              "and/or slice pitch) are not supported by device associated "
              "with queue";
          break;
      case CL_OUT_OF_RESOURCES:
          msg = "(1) There is a failure to queue the execution instance of "
              "kernel on the command-queue because of insufficient resources"
              " needed to execute the kernel. (2) There is a failure to "
              "allocate resources required by the OpenCL implementation "
              "on the device.";
          break;
      case CL_MEM_OBJECT_ALLOCATION_FAILURE:
          msg = "There is a failure to allocate memory for data store "
              "associated with image or buffer objects specified as "
              "arguments to kernel.";
          break;
      case CL_INVALID_EVENT_WAIT_LIST:
          msg = "Event_wait_list is NULL and num_events_in_wait_list > 0, or "
              "event_wait_list is not NULL and num_events_in_wait_list is 0, "
              "or if event objects in event_wait_list are not valid events. ";
          break;
      case CL_OUT_OF_HOST_MEMORY:
          msg = "There is a failure to allocate resources required by the "
              "OpenCL implementation on the host.";
          break;
      default: msg = "Unknown error"; break;
      }

      casadi_error("clEnqueueTask failed: " << msg);
    }
  }

#endif // WITH_OPENCL

} // namespace casadi
