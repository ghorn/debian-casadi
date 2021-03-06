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
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
*    Lesser General Public License for more details.
*
*    You should have received a copy of the GNU Lesser General Public
*    License along with CasADi; if not, write to the Free Software
*    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*
*/

#include "mx_function_internal.hpp"
#include "../mx/call_function.hpp"
#include "../mx/mx_tools.hpp"
#include "../sx/sx_tools.hpp"

#include "../std_vector_tools.hpp"
#include "../casadi_types.hpp"

#include <stack>
#include <typeinfo>
#include "../profiling.hpp"
#include "../casadi_options.hpp"

using namespace std;

namespace casadi {

  MXFunctionInternal::MXFunctionInternal(const std::vector<MX>& inputv,
                                         const std::vector<MX>& outputv) :
    XFunctionInternal<MXFunction, MXFunctionInternal, MX, MXNode>(inputv, outputv) {

    setOption("name", "unnamed_mx_function");

    // Check for inputs that are not symbolic primitives
    int ind=0;
    for (vector<MX>::iterator it = inputv_.begin(); it!=inputv_.end(); ++it, ++ind) {
      if (!it->isSymbolic()) {
        if (it->isEmpty()) {
          stringstream ss;
          ss << "r" << ind;
          *it = MX::sym(ss.str(), it->sparsity());
        } else {
          casadi_error(
            "Failed to create an MXFunction instance since not all input "
            "arguments are symbolic primitives. Support for non-symbolic "
            "inputs has been dropped. We refer users to the approach "
            "demonstrated in "
            "http://docs.casadi.org/tutorials/tools/structure.pdf");
        }
      }
    }

    // Check for duplicate entries among the input expressions
    bool has_duplicates = false;
    for (vector<MX>::iterator it = inputv_.begin(); it != inputv_.end(); ++it) {
      has_duplicates = has_duplicates || it->getTemp()!=0;
      it->setTemp(1);
    }

    // Reset temporaries
    for (vector<MX>::iterator it = inputv_.begin(); it != inputv_.end(); ++it) {
      it->setTemp(0);
    }
    casadi_assert_message(!has_duplicates, "The input expressions are not independent.");
  }


  MXFunctionInternal::~MXFunctionInternal() {
  }


  void MXFunctionInternal::init() {
    log("MXFunctionInternal::init begin");

    // Call the init function of the base class
    XFunctionInternal<MXFunction, MXFunctionInternal, MX, MXNode>::init();

    // Stack used to sort the computational graph
    stack<MXNode*> s;

    // All nodes
    vector<MXNode*> nodes;

    // Add the list of nodes
    int ind=0;
    for (vector<MX>::iterator it = outputv_.begin(); it != outputv_.end(); ++it, ++ind) {
      // Add outputs to the list
      s.push(static_cast<MXNode*>(it->get()));
      sort_depth_first(s, nodes);

      // A null pointer means an output instruction
      nodes.push_back(static_cast<MXNode*>(0));
    }

    // Make sure that all inputs have been added also // TODO REMOVE THIS
    for (vector<MX>::iterator it = inputv_.begin(); it != inputv_.end(); ++it) {
      if (!it->getTemp()) {
        nodes.push_back(static_cast<MXNode*>(it->get()));
      }
    }

    // Set the temporary variables to be the corresponding place in the sorted graph
    for (int i=0; i<nodes.size(); ++i) {
      if (nodes[i]) {
        nodes[i]->temp = i;
      }
    }

    // Place in the algorithm for each node
    vector<int> place_in_alg;
    place_in_alg.reserve(nodes.size());

    // Use live variables?
    bool live_variables = getOption("live_variables");

    // Input instructions
    vector<pair<int, MXNode*> > symb_loc;

    // Current output and nonzero, start with the first one
    int curr_oind=0;

    // Count the number of times each node is used
    vector<int> refcount(nodes.size(), 0);

    // Get the sequence of instructions for the virtual machine
    algorithm_.resize(0);
    algorithm_.reserve(nodes.size());
    for (vector<MXNode*>::iterator it=nodes.begin(); it!=nodes.end(); ++it) {
      // Current node
      MXNode* n = *it;

      // Get the operation
      int op = n==0 ? OP_OUTPUT : n->getOp();

      // Store location if parameter (or input)
      if (op==OP_PARAMETER) {
        symb_loc.push_back(make_pair(algorithm_.size(), n));
      }

      // If a new element in the algorithm needs to be added
      if (op>=0) {
        AlgEl ae;
        ae.op = op;
        ae.data.assignNode(n);

        // Add input and output argument
        if (op==OP_OUTPUT) {
          ae.arg.resize(1);
          ae.arg[0] = outputv_.at(curr_oind)->temp;
          ae.res.resize(1);
          ae.res[0] = curr_oind++;
        } else {
          ae.arg.resize(n->ndep());
          for (int i=0; i<n->ndep(); ++i) {
            ae.arg[i] = n->dep(i)->temp;
          }
          ae.res.resize(n->getNumOutputs());
          if (n->isMultipleOutput()) {
            fill(ae.res.begin(), ae.res.end(), -1);
          } else {
            ae.res[0] = n->temp;
          }
        }

        // Increase the reference count of the dependencies
        for (int c=0; c<ae.arg.size(); ++c) {
          if (ae.arg[c]>=0) {
            refcount[ae.arg[c]]++;
          }
        }

        // Save to algorithm
        place_in_alg.push_back(algorithm_.size());
        algorithm_.push_back(ae);

      } else { // Function output node
        // Get the output index
        int oind = n->getFunctionOutput();

        // Get the index of the parent node
        int pind = place_in_alg[n->dep(0)->temp];

        // Save location in the algorithm element corresponding to the parent node
        int& otmp = algorithm_[pind].res.at(oind);
        if (otmp<0) {
          otmp = n->temp; // First time this function output is encountered, save to algorithm
        } else {
          n->temp = otmp; // Function output is a duplicate, use the node encountered first
        }

        // Not in the algorithm
        place_in_alg.push_back(-1);
      }
    }

    // Place in the work vector for each of the nodes in the tree (overwrites the reference counter)
    vector<int>& place = place_in_alg; // Reuse memory as it is no longer needed
    place.resize(nodes.size());

    // Stack with unused elements in the work vector, sorted by sparsity pattern
    SPARSITY_MAP<const void*, stack<int> > unused_all;

    // Work vector size
    int worksize = 0;

    // Find a place in the work vector for the operation
    for (vector<AlgEl>::iterator it=algorithm_.begin(); it!=algorithm_.end(); ++it) {

      // There are two tasks, allocate memory of the result and free the
      // memory off the arguments, order depends on whether inplace is possible
      int first_to_free = 0;
      int last_to_free = it->op==OP_OUTPUT ? 1 : it->data->numInplace();
      for (int task=0; task<2; ++task) {

        // Dereference or free the memory of the arguments
        for (int c=last_to_free-1; c>=first_to_free; --c) { // reverse order so that the
                                                          // first argument will end up
                                                          // at the top of the stack

          // Index of the argument
          int& ch_ind = it->arg[c];
          if (ch_ind>=0) {

            // Decrease reference count and add to the stack of
            // unused variables if the count hits zero
            int remaining = --refcount[ch_ind];

            // Free variable for reuse
            if (live_variables && remaining==0) {

              // Get a pointer to the sparsity pattern of the argument that can be freed
              const void* sp = nodes[ch_ind]->sparsity().get();

              // Add to the stack of unused work vector elements for the current sparsity
              unused_all[sp].push(place[ch_ind]);
            }

            // Point to the place in the work vector instead of to the place in the list of nodes
            ch_ind = place[ch_ind];
          }
        }

        // Nothing more to allocate
        if (it->op==OP_OUTPUT || task==1) break;

        // Free the rest in the next iteration
        first_to_free = last_to_free;
        last_to_free = it->arg.size();

        // Allocate/reuse memory for the results of the operation
        for (int c=0; c<it->res.size(); ++c) {
          if (it->res[c]>=0) {

            // Are reuse of variables (live variables) enabled?
            if (live_variables) {
              // Get a pointer to the sparsity pattern node
              const void* sp = it->data->sparsity(c).get();

              // Get a reference to the stack for the current sparsity
              stack<int>& unused = unused_all[sp];

              // Try to reuse a variable from the stack if possible (last in, first out)
              if (!unused.empty()) {
                it->res[c] = place[it->res[c]] = unused.top();
                unused.pop();
                continue; // Success, no new element needed in the work vector
              }
            }

            // Allocate a new element in the work vector
            it->res[c] = place[it->res[c]] = worksize++;
          }
        }
      }
    }

    if (verbose()) {
      if (live_variables) {
        cout << "Using live variables: work array is "
             <<  worksize << " instead of "
             << nodes.size() << endl;
      } else {
        cout << "Live variables disabled." << endl;
      }
    }

    // Allocate work vectors (numeric)
    work_.resize(0);
    work_.resize(worksize, make_pair(DMatrix(), 0));
    size_t nitmp=0, nrtmp=0;
    for (vector<AlgEl>::iterator it=algorithm_.begin(); it!=algorithm_.end(); ++it) {
      if (it->op!=OP_OUTPUT) {
        for (int c=0; c<it->res.size(); ++c) {
          if (it->res[c]>=0) {
            size_t nr=0, ni=0;
            it->data->nTmp(ni, nr);
            nitmp = std::max(nitmp, ni);
            nrtmp = std::max(nrtmp, nr);
            if (work_[it->res[c]].first.isEmpty()) {
              work_[it->res[c]].first = Matrix<double>(it->data->sparsity(c), 0);
            }
          }
        }
      }
    }
    itmp_.resize(nitmp);
    rtmp_.resize(nrtmp);

    // Reset the temporary variables
    for (int i=0; i<nodes.size(); ++i) {
      if (nodes[i]) {
        nodes[i]->temp = 0;
      }
    }

    // Now mark each input's place in the algorithm
    for (vector<pair<int, MXNode*> >::const_iterator it=symb_loc.begin();
         it!=symb_loc.end(); ++it) {
      it->second->temp = it->first+1;
    }

    // Add input instructions
    for (int ind=0; ind<inputv_.size(); ++ind) {
      int i = inputv_[ind].getTemp()-1;
      if (i>=0) {
        // Mark as input
        algorithm_[i].op = OP_INPUT;

        // Location of the input
        algorithm_[i].arg = vector<int>(1, ind);

        // Mark input as read
        inputv_[ind].setTemp(0);
      }
    }

    // Locate free variables
    free_vars_.clear();
    for (vector<pair<int, MXNode*> >::const_iterator it=symb_loc.begin();
         it!=symb_loc.end(); ++it) {
      int i = it->second->temp-1;
      if (i>=0) {
        // Save to list of free parameters
        free_vars_.push_back(MX::create(it->second));

        // Remove marker
        it->second->temp=0;
      }
    }

    if (CasadiOptions::profiling && CasadiOptions::profilingBinary) {
      profileWriteName(CasadiOptions::profilingLog, this, getOption("name"),
                       ProfilingData_FunctionType_MXFunction, algorithm_.size());
      int alg_counter = 0;
      for (vector<AlgEl>::iterator it=algorithm_.begin(); it!=algorithm_.end();
           ++it, ++alg_counter) {
        std::stringstream ss;
        print(ss, *it);
        profileWriteSourceLine(CasadiOptions::profilingLog, this, alg_counter, ss.str(), it->op,
                               (it->op == OP_CALL)? it->data->getFunction().operator->() : 0);
      }
    }

    log("MXFunctionInternal::init end");
  }

  void MXFunctionInternal::updatePointers(const AlgEl& el) {
    mx_input_.resize(el.arg.size());
    mx_output_.resize(el.res.size());

    if (el.op!=OP_INPUT) {
      for (int i=0; i<mx_input_.size(); ++i) {
        if (el.arg[i]>=0) {
          int k = el.arg[i];
          mx_input_[i] = &work_[k].first;
        } else {
          mx_input_[i] = 0;
        }
      }
    }

    if (el.op!=OP_OUTPUT) {
      for (int i=0; i<mx_output_.size(); ++i) {
        if (el.res[i]>=0) {
          mx_output_[i] = &work_[el.res[i]].first;
        } else {
          mx_output_[i] = 0;
        }
      }
    }
  }

  void MXFunctionInternal::evaluate() {
    casadi_log("MXFunctionInternal::evaluate():begin "  << getOption("name"));
    // Set up timers for profiling
    double time_zero=0;
    double time_start=0;
    double time_stop=0;
    if (CasadiOptions::profiling) {
      time_zero = getRealTime();
      if (CasadiOptions::profilingBinary) {
        profileWriteEntry(CasadiOptions::profilingLog, this);
      } else {
        CasadiOptions::profilingLog  << "start " << this << ":" <<getOption("name") << std::endl;
      }
    }

    // Make sure that there are no free variables
    if (!free_vars_.empty()) {
      std::stringstream ss;
      repr(ss);
      casadi_error("Cannot evaluate \"" << ss.str() << "\" since variables "
                   << free_vars_ << " are free.");
    }

    // Evaluate all of the nodes of the algorithm:
    // should only evaluate nodes that have not yet been calculated!
    int alg_counter = 0;
    for (vector<AlgEl>::iterator it=algorithm_.begin(); it!=algorithm_.end(); ++it, ++alg_counter) {
      if (CasadiOptions::profiling) {
        time_start = getRealTime(); // Start timer
      }

      if (it->op==OP_INPUT) {
        // Pass an input
        work_[it->res.front()].first.set(input(it->arg.front()));
      } else if (it->op==OP_OUTPUT) {
        // Get an output
        work_[it->arg.front()].first.get(output(it->res.front()));
      } else {

        // Point pointers to the data corresponding to the element
        updatePointers(*it);

        // Evaluate
        it->data->evaluateD(mx_input_, mx_output_, itmp_, rtmp_);

      }

      // Write out profiling information
      if (CasadiOptions::profiling) {
        time_stop = getRealTime(); // Stop timer

        if (CasadiOptions::profilingBinary) {
          profileWriteTime(CasadiOptions::profilingLog, this, alg_counter,
                           time_stop-time_start, time_stop-time_zero);
        } else {
          CasadiOptions::profilingLog  << (time_stop-time_start)*1e6 << " ns | "
                                       << (time_stop-time_zero)*1e3 << " ms | "
                                       << this << ":" <<getOption("name") << ":"
                                       << alg_counter <<"|";
          if (it->op == OP_CALL) {
            Function f = it->data->getFunction();
            CasadiOptions::profilingLog << f.get() << ":" << f.getOption("name");
          }
          CasadiOptions::profilingLog << "|";
          print(CasadiOptions::profilingLog, *it);
        }

      }
    }

    if (CasadiOptions::profiling) {
      time_stop = getRealTime();
      if (CasadiOptions::profilingBinary) {
        profileWriteExit(CasadiOptions::profilingLog, this, time_stop-time_zero);
      } else {
        CasadiOptions::profilingLog  << "stop " << this << ":"
                                     <<getOption("name") << (time_stop-time_zero)*1e3
                                     << " ms" << std::endl;
      }
    }

    casadi_log("MXFunctionInternal::evaluate():end "  << getOption("name"));
  }

  void MXFunctionInternal::print(ostream &stream, const AlgEl& el) const {
    if (el.op==OP_OUTPUT) {
      stream << "output[" << el.res.front() << "] = @" << el.arg.at(0);
    } else if (el.op==OP_SETNONZEROS || el.op==OP_ADDNONZEROS) {
      if (el.res.front()!=el.arg.at(0)) {
        stream << "@" << el.res.front() << " = @" << el.arg.at(0) << "; ";
      }
      stream << "@" << el.res.front();
      el.data->printPart(stream, 1);
      stream << "@" << el.arg.at(1);
    } else {
      if (el.res.size()==1) {
        stream << "@" << el.res.front() << " = ";
      } else {
        stream << "{";
        for (int i=0; i<el.res.size(); ++i) {
          if (i!=0) stream << ", ";
          if (el.res[i]>=0) {
            stream << "@" << el.res[i];
          } else {
            stream << "NULL";
          }
        }
        stream << "} = ";
      }
      if (el.op==OP_INPUT) {
        stream << "input[" << el.arg.front() << "]";
      } else {
        el.data->printPart(stream, 0);
        for (int i=0; i<el.arg.size(); ++i) {
          if (el.arg[i]>=0) {
            stream << "@" << el.arg[i];
          } else {
            stream << "NULL";
          }
          el.data->printPart(stream, i+1);
        }
      }
    }
    stream << endl;
  }

  void MXFunctionInternal::print(ostream &stream) const {
    FunctionInternal::print(stream);
    for (vector<AlgEl>::const_iterator it=algorithm_.begin(); it!=algorithm_.end(); ++it) {
      print(stream, *it);
    }
  }

  MXFunctionInternal* MXFunctionInternal::clone() const {
    return new MXFunctionInternal(*this);
  }

  void MXFunctionInternal::deepCopyMembers(
      std::map<SharedObjectNode*, SharedObject>& already_copied) {
    XFunctionInternal<MXFunction, MXFunctionInternal, MX, MXNode>::deepCopyMembers(already_copied);
    for (vector<AlgEl>::iterator it=algorithm_.begin(); it!=algorithm_.end(); ++it) {
      switch (it->op) {
      case OP_CALL:
      case OP_SOLVE:
        it->data.makeUnique(already_copied, false);
        it->data->getFunction() = deepcopy(it->data->getFunction(), already_copied);
        break;
      default:
        break;
      }
    }
  }

  void MXFunctionInternal::spInit(bool fwd) {
    // Start by setting all elements of the work vector to zero
    for (vector<pair<DMatrix, int> >::iterator it=work_.begin(); it!=work_.end(); ++it) {
      //Get a pointer to the int array
      bvec_t *iwork = get_bvec_t(it->first.data());
      fill_n(iwork, it->first.size(), bvec_t(0));
    }
  }

  void MXFunctionInternal::spEvaluate(bool fwd) {
    if (fwd) { // Forward propagation

      // Propagate sparsity forward
      for (vector<AlgEl>::iterator it=algorithm_.begin(); it!=algorithm_.end(); it++) {
        if (it->op==OP_INPUT) {
          // Pass input seeds
          vector<double> &w = work_[it->res.front()].first.data();
          bvec_t* iwork = get_bvec_t(w);
          bvec_t* swork = get_bvec_t(input(it->arg.front()).data());
          copy(swork, swork+w.size(), iwork);
        } else if (it->op==OP_OUTPUT) {
          // Get the output sensitivities
          vector<double> &w = work_[it->arg.front()].first.data();
          bvec_t* iwork = get_bvec_t(w);
          bvec_t* swork = get_bvec_t(output(it->res.front()).data());
          copy(iwork, iwork+w.size(), swork);
        } else {
          // Point pointers to the data corresponding to the element
          updatePointers(*it);

          // Propagate sparsity forwards
          it->data->propagateSparsity(mx_input_, mx_output_, itmp_, rtmp_, true);
        }
      }

    } else { // Backward propagation

      // Propagate sparsity backwards
      for (vector<AlgEl>::reverse_iterator it=algorithm_.rbegin(); it!=algorithm_.rend(); it++) {
        if (it->op==OP_INPUT) {
          // Get the input sensitivities and clear it from the work vector
          vector<double> &w = work_[it->res.front()].first.data();
          bvec_t* iwork = get_bvec_t(w);
          bvec_t* swork = get_bvec_t(input(it->arg.front()).data());
          for (int k=0; k<w.size(); ++k) {
            swork[k] = iwork[k];
            iwork[k] = 0;
          }
        } else if (it->op==OP_OUTPUT) {
          // Pass output seeds
          vector<double> &w = work_[it->arg.front()].first.data();
          bvec_t* iwork = get_bvec_t(w);
          bvec_t* swork = get_bvec_t(output(it->res.front()).data());
          for (int k=0; k<w.size(); ++k) {
            iwork[k] |= swork[k];
          }
        } else {
          // Point pointers to the data corresponding to the element
          updatePointers(*it);

          // Propagate sparsity backwards
          it->data->propagateSparsity(mx_input_, mx_output_, itmp_, rtmp_, false);
        }
      }
    }
  }

  Function MXFunctionInternal::getNumericJacobian(int iind, int oind,
                                                  bool compact, bool symmetric) {
    // Create expressions for the Jacobian
    vector<MX> ret_out;
    ret_out.reserve(1+outputv_.size());
    ret_out.push_back(jac(iind, oind, compact, symmetric, false, true));
    ret_out.insert(ret_out.end(), outputv_.begin(), outputv_.end());

    MXFunction ret(inputv_, ret_out);
    ret.setInputScheme(inputScheme());
    // Return function
    return ret;
  }

  std::vector<MX> MXFunctionInternal::symbolicOutput(const std::vector<MX>& arg) {
    // Check if input is given
    const int checking_depth = 2;
    bool input_given = true;
    for (int i=0; i<arg.size() && input_given; ++i) {
      if (!isEqual(arg[i], inputv_[i], checking_depth)) {
        input_given = false;
      }
    }

    // Return output if possible, else fall back to base class
    if (input_given) {
      return outputv_;
    } else {
      return FunctionInternal::symbolicOutput(arg);
    }
  }

  void MXFunctionInternal::evalMX(const std::vector<MX>& arg1, std::vector<MX>& res1,
                                  const std::vector<std::vector<MX> >& fseed,
                                  std::vector<std::vector<MX> >& fsens,
                                  const std::vector<std::vector<MX> >& aseed,
                                  std::vector<std::vector<MX> >& asens) {
    log("MXFunctionInternal::evalMX begin");
    assertInit();
    casadi_assert_message(arg1.size()==getNumInputs(), "Wrong number of input arguments");

    // Resize the number of outputs
    res1.resize(outputv_.size());

    // Check if arguments matches the input expressions, in which case
    // the output is known to be the output expressions
    const int checking_depth = 2;
    bool output_given = true;
    for (int i=0; i<arg1.size() && output_given; ++i) {
      if (!isEqual(arg1[i], inputv_[i], checking_depth)) {
        output_given = false;
      }
    }

    // Copy output if known
    if (output_given) {
      copy(outputv_.begin(), outputv_.end(), res1.begin());
    }

    // Use the function arguments if possible to avoid problems involving
    // equivalent but different expressions
    const vector<MX>& arg = output_given ? inputv_ : arg1;
    vector<MX>& res = output_given ? outputv_ : res1;

    // Skip forward sensitivities if no nonempty seeds
    bool skip_fwd = true;
    for (vector<vector<MX> >::const_iterator i=fseed.begin(); i!=fseed.end() && skip_fwd; ++i) {
      for (vector<MX>::const_iterator j=i->begin(); j!=i->end() && skip_fwd; ++j) {
        if (j->size()>0) {
          skip_fwd = false;
        }
      }
    }

    // Skip forward sensitivities if no nonempty seeds
    bool skip_adj = true;
    for (vector<vector<MX> >::const_iterator i=aseed.begin(); i!=aseed.end() && skip_adj; ++i) {
      for (vector<MX>::const_iterator j=i->begin(); j!=i->end() && skip_adj; ++j) {
        if (j->size()>0) {
          skip_adj = false;
        }
      }
    }

    // Get the number of directions
    int nfdir = fseed.size();
    int nadir = aseed.size();

    // Allocate outputs
    if (!output_given) {
      res.resize(outputv_.size());
    }

    // Temporary vector to hold function outputs
    vector<MX> output_tmp;

    // Allocate forward sensitivities
    fsens.resize(nfdir);
    for (int d=0; d<nfdir; ++d) {
      fsens[d].resize(outputv_.size());
      if (skip_fwd) {
        for (int i=0; i<fsens[d].size(); ++i) {
          fsens[d][i] = MX::sparse(output(i).shape());
        }
      }
    }

    // Skip if trivial
    if (skip_fwd) nfdir = 0;

    // Allocate adjoint sensitivities
    asens.resize(nadir);
    for (int d=0; d<nadir; ++d) {
      asens[d].resize(inputv_.size());
      if (skip_adj) {
        for (int i=0; i<asens[d].size(); ++i) {
          asens[d][i] = MX::sparse(input(i).shape());
        }
      }
    }

    // Skip if trivial
    if (skip_adj) nadir = 0;

    // Quick return if nothing to calculate
    if (output_given && nfdir==0 && nadir==0) {
      log("MXFunctionInternal::evalMX quick return");
      return;
    }

    // Symbolic work, non-differentiated
    vector<MX> swork(work_.size());
    log("MXFunctionInternal::evalMX allocated work vector");

    // "Tape" with spilled variables
    vector<pair<pair<int, int>, MX> > tape;
    allocTape(tape);

    // Tape counter
    int tt = 0;

    MXPtrV input_p, output_p;
    MXPtrVV fseed_p(nfdir), fsens_p(nfdir);
    MXPtrVV aseed_p(nadir), asens_p(nadir);
    MXPtrVV fseed_purged(nfdir), fsens_purged(nfdir);
    MXPtrVV aseed_purged(nadir), asens_purged(nadir);
    MXPtrVV dummy_p;

    // Work vector, forward derivatives
    std::vector<std::vector<MX> > dwork(work_.size());
    fill(dwork.begin(), dwork.end(), std::vector<MX>(nfdir));
    log("MXFunctionInternal::evalMX allocated derivative work vector (forward mode)");

    // Loop over computational nodes in forward order
    int alg_counter = 0;
    for (vector<AlgEl>::iterator it=algorithm_.begin(); it!=algorithm_.end(); ++it, ++alg_counter) {

      // Spill existing work elements if needed
      if (nadir>0 && it->op!=OP_OUTPUT) {
        for (vector<int>::const_iterator c=it->res.begin(); c!=it->res.end(); ++c) {
          if (*c >=0 && tt<tape.size() && tape[tt].first == make_pair(alg_counter, *c)) {
            tape[tt++].second = swork[*c];
          }
        }
      }

      if (it->op == OP_INPUT) {
        // Fetch input
        const Sparsity& sp_input = input(it->arg.front()).sparsity();
        swork[it->res.front()] = arg[it->arg.front()].setSparse(sp_input, true);
        for (int d=0; d<nfdir; ++d) {
          dwork[it->res.front()][d] = fseed[d][it->arg.front()].setSparse(sp_input, true);
        }
      } else if (it->op==OP_OUTPUT) {
        // Collect the results
        if (!output_given) {
          res[it->res.front()] = swork[it->arg.front()];
        }

        // Collect the forward sensitivities
        for (int d=0; d<nfdir; ++d) {
          fsens[d][it->res.front()] = dwork[it->arg.front()][d];
        }
      } else if (it->op==OP_PARAMETER) {
        // Fetch parameter
        swork[it->res.front()] = it->data;
        for (int d=0; d<nfdir; ++d) {
          dwork[it->res.front()][d] = MX();
        }
      } else {

        // Get expressions for the result of the operation, if known
        if (output_given) {
          output_tmp.resize(it->res.size());
          for (int i=0; i<it->res.size(); ++i) {
            if (it->res[i]>=0) {
              output_tmp[i] = it->data.getOutput(i);
            }
          }
        }

        // Pointers to the arguments of the evaluation
        input_p.resize(it->arg.size());
        for (int i=0; i<input_p.size(); ++i) {
          int el = it->arg[i]; // index of the argument
          input_p[i] = el<0 ? 0 : &swork[el];
        }

        // Pointers to the result of the evaluation
        output_p.resize(it->res.size());
        for (int i=0; i<output_p.size(); ++i) {
          int el = it->res[i]; // index of the output
          output_p[i] = el<0 ? 0 : output_given ? &output_tmp[i] : &swork[el];
        }

        // Forward seeds and sensitivities
        for (int d=0; d<nfdir; ++d) {
          fseed_p[d].resize(it->arg.size());
          for (int iind=0; iind<it->arg.size(); ++iind) {
            int el = it->arg[iind];
            fseed_p[d][iind] = el<0 ? 0 : &dwork[el][d];

            // Give zero seed if null
            if (el>=0 && dwork[el][d].isEmpty(true)) {
              if (d==0) {
                dwork[el][d] = MX::sparse(input_p[iind]->shape());
              } else {
                dwork[el][d] = dwork[el][0];
              }
            }
          }

          fsens_p[d].resize(it->res.size());
          for (int oind=0; oind<it->res.size(); ++oind) {
            int el = it->res[oind];
            fsens_p[d][oind] = el<0 ? 0 : &dwork[el][d];
            if (el>=0 && dwork[el][d].isEmpty(true)) {
              dwork[el][d] = MX::sparse(output_p[oind]->shape());
            }
          }
        }

        // Call the evaluation function
        if (!output_given || nfdir>0) {
          if (it->data->getOp()==OP_CALL) {
            // Purge the directions that have all-zero seeds #905
            // We do this only for OP_CALL since some operations might have
            // a substancial effect on the sensitivities even though all seeds are zero:
            // the sparsity might be changed e.g. in OP_SETSPARSE
            purgeSeeds(fseed_p, fsens_p, fseed_purged, fsens_purged, true);
            if (fsens_purged.size()==0 && fsens_purged.size()==0) {
              it->data->evaluateMX(input_p, output_p, dummy_p, dummy_p,
                                   dummy_p, dummy_p, output_given);
            } else {
              if (CasadiOptions::purgeSeeds) {
                // Call the evaluation function
                it->data->evaluateMX(input_p, output_p, fseed_purged, fsens_purged,
                                     dummy_p, dummy_p, output_given);
              } else {
                // Do nothing special
                it->data->evaluateMX(input_p, output_p, fseed_p, fsens_p,
                                     dummy_p, dummy_p, output_given);
              }
            }
          } else {
            // Call the evaluation function
            it->data->evaluateMX(input_p, output_p, fseed_p, fsens_p,
                                 dummy_p, dummy_p, output_given);
          }
        }

        // Save results of the operation to work vector,
        // if known (not earlier to allow inplace operations)
        if (output_given) {
          for (int i=0; i<it->res.size(); ++i) {
            int el = it->res[i]; // index of the output
            if (el>=0) swork[el] = output_tmp[i];
          }
        }
      }
    }

    // Loop over computational nodes in reverse order
    if (nadir>0) {
      // Work vector, adjoint derivatives
      fill(dwork.begin(), dwork.end(), std::vector<MX>(nadir));
      log("MXFunctionInternal::evalMX allocated derivative work vector (adjoint mode)");

      int alg_counter = algorithm_.size()-1;
      tt--;
      for (vector<AlgEl>::reverse_iterator it=algorithm_.rbegin();
          it!=algorithm_.rend();
          ++it, --alg_counter) {
        // Mark spilled work vector elements to be recovered to allow the
        // operator input to be updated but not the operator output
        // (important for inplace operations)
        if (it->op!=OP_OUTPUT) {
          for (vector<int>::const_reverse_iterator c=it->res.rbegin(); c!=it->res.rend(); ++c) {
            if (*c >=0 && tt>=0 && tape[tt].first==make_pair(alg_counter, *c)) {
              work_[*c].second = 1 + tt--;
            }
          }
        }

        if (it->op == OP_INPUT) {
          // Collect the symbolic adjoint sensitivities
          for (int d=0; d<nadir; ++d) {
            if (dwork[it->res.front()][d].isEmpty(true)) {
              asens[d][it->arg.front()] = MX::sparse(input(it->arg.front()).shape());
            } else {
              asens[d][it->arg.front()] = dwork[it->res.front()][d];
            }
            dwork[it->res.front()][d] = MX();
          }
        } else if (it->op==OP_OUTPUT) {
          // Pass the adjoint seeds
          for (int d=0; d<nadir; ++d) {
            dwork[it->arg.front()][d].addToSum(
              aseed[d][it->res.front()].setSparse(output(it->res.front()).sparsity(), true));
          }
        } else if (it->op==OP_PARAMETER) {
          // Clear adjoint seeds
          for (int d=0; d<nadir; ++d) {
            dwork[it->res.front()][d] = MX();
          }
        } else {
          // Get the arguments of the evaluation
          input_p.resize(it->arg.size());
          for (int i=0; i<input_p.size(); ++i) {
            int el = it->arg[i]; // index of the argument
            if (el<0) {
              input_p[i] = 0;
            } else {
              int tmp = work_[el].second; // Positive if the data should be retrieved from the
                                          // tape instead of the work vector
              input_p[i] = tmp==0 ? &swork[el] : &tape[tmp-1].second;
            }
          }

          // Result of the evaluation
          output_p.resize(it->res.size());
          for (int i=0; i<output_p.size(); ++i) {
            int el = it->res[i]; // index of the output
            output_p[i] = el<0 ? 0 : &swork[el];
          }

          // Sensitivity arguments
          for (int d=0; d<nadir; ++d) {
            aseed_p[d].resize(it->res.size());
            for (int oind=0; oind<it->res.size(); ++oind) {
              int el = it->res[oind];
              aseed_p[d][oind] = el<0 ? 0 : &dwork[el][d];

              // Provide a zero seed if no seed exists
              if (el>=0 && dwork[el][d].isEmpty(true)) {
                dwork[el][d] = MX::sparse(swork[el].shape());
              }
            }

            asens_p[d].resize(it->arg.size());
            for (int iind=0; iind<it->arg.size(); ++iind) {
              int el = it->arg[iind];
              asens_p[d][iind] = el<0 ? 0 : &dwork[el][d];

              // Set sensitivities to zero if not yet used
              if (el>=0 && dwork[el][d].isEmpty(true)) {
                dwork[el][d] = MX::sparse(swork[el].shape());
              }
            }
          }


          if (it->data->getOp()==OP_CALL) {
            // Purge the directions that have all-zero seeds #905
            // We do this only for OP_CALL since some operations might have a substancial effect
            // on the sensitivities even though all seeds are zero: the sparisty might be
            // changed e.g. in OP_SETSPARSE
            purgeSeeds(aseed_p, asens_p, aseed_purged, asens_purged, false);
            if (aseed_purged.size()==0 && asens_purged.size()==0) {
              // Call the evaluation function
              it->data->evaluateMX(input_p, output_p, dummy_p, dummy_p, dummy_p, dummy_p, true);
            } else {
              if (CasadiOptions::purgeSeeds) {
                // Call the evaluation function
                it->data->evaluateMX(input_p, output_p, dummy_p,
                                     dummy_p, aseed_purged, asens_purged, true);
              } else {
                // Do nothing special
                it->data->evaluateMX(input_p, output_p, dummy_p, dummy_p, aseed_p, asens_p, true);
              }
            }
          } else {
            // Call the evaluation function
            it->data->evaluateMX(input_p, output_p, dummy_p, dummy_p, aseed_p, asens_p, true);
          }
        }

        // Recover the spilled elements to the work vector for later access
        // (delayed for inplace operations)
        if (it->op!=OP_OUTPUT) {
          for (vector<int>::const_reverse_iterator c=it->res.rbegin(); c!=it->res.rend(); ++c) {
            if (*c >=0 && work_[*c].second > 0) {
              swork[*c] = tape[work_[*c].second-1].second;
              work_[*c].second = 0;
            }
          }
        }
      }
    }
    log("MXFunctionInternal::evalMX end");
  }

  void MXFunctionInternal::evalSXsparse(const std::vector<SX>& input_s, std::vector<SX>& output_s,
                                        const std::vector<std::vector<SX> >& fwdSeed,
                                        std::vector<std::vector<SX> >& fwdSens,
                                        const std::vector<std::vector<SX> >& adjSeed,
                                        std::vector<std::vector<SX> >& adjSens) {
    casadi_assert_message(fwdSens.empty(), "Not implemented");
    casadi_assert_message(adjSeed.empty(), "Not implemented");

    // Create a work array
    vector<SX> swork(work_.size());
    for (vector<AlgEl>::iterator it=algorithm_.begin(); it!=algorithm_.end(); it++) {
      if (it->op!=OP_OUTPUT) {
        for (int i=0; i<it->res.size(); ++i) {
          if (it->res[i]>=0)
            swork[it->res[i]] = SX(it->data->sparsity(i));
        }
      }
    }

    // Create a temporary vector
    vector<SXElement> rtmp(rtmp_.size());

    // Evaluate all of the nodes of the algorithm:
    // should only evaluate nodes that have not yet been calculated!
    vector<SX*> sxarg;
    vector<SX*> sxres;
    for (vector<AlgEl>::iterator it=algorithm_.begin(); it!=algorithm_.end(); it++) {
      if (it->op==OP_INPUT) {
        // Pass the input
        swork[it->res.front()].set(input_s[it->arg.front()]);
      } else if (it->op==OP_OUTPUT) {
        // Get the outputs
        swork[it->arg.front()].get(output_s[it->res.front()]);
      } else if (it->op==OP_PARAMETER) {
        continue; // FIXME
      } else {
        sxarg.resize(it->arg.size());
        for (int c=0; c<sxarg.size(); ++c) {
          int ind = it->arg[c];
          sxarg[c] = ind<0 ? 0 : &swork[ind];
        }
        sxres.resize(it->res.size());
        for (int c=0; c<sxres.size(); ++c) {
          int ind = it->res[c];
          sxres[c] = ind<0 ? 0 : &swork[ind];
        }
        it->data->evaluateSX(sxarg, sxres, itmp_, rtmp);
      }
    }
  }

  SXFunction MXFunctionInternal::expand(const std::vector<SX>& inputvsx) {
    assertInit();

    // Create inputs with the same name and sparsity as the matrix valued symbolic inputs
    vector<SX> arg(inputv_.size());
    if (inputvsx.empty()) { // No symbolic input provided
      for (int i=0; i<arg.size(); ++i) {
        arg[i] = SX::sym(inputv_[i]->getName(), inputv_[i].sparsity());
      }
    } else { // Use provided symbolic input
      // Make sure number of inputs matches
      casadi_assert(inputvsx.size()==inputv_.size());

      // Make sure that sparsity matches
      for (int i=0; i<inputvsx.size(); ++i) {
        casadi_assert(inputvsx[i].sparsity() == inputv_[i].sparsity());
      }

      // Copy to argument vector
      copy(inputvsx.begin(), inputvsx.end(), arg.begin());
    }

    // Create output vector with correct sparsity
    vector<SX> res(outputv_.size());
    for (int i=0; i<res.size(); ++i) {
      res[i] = SX(outputv_[i].sparsity());
    }

    // No sensitivities
    vector<vector<SX> > dummy;

    // Evaluate symbolically
    evalSX(arg, res, dummy, dummy, dummy, dummy);

    // Create function
    SXFunction f(arg, res);
    f.setInputScheme(getInputScheme());
    f.setOutputScheme(getOutputScheme());
    string name = getOption("name");
    f.setOption("name", "expand_" + name);
    return f;
  }

  void MXFunctionInternal::printWork(ostream &stream) {
    for (int k=0; k<work_.size(); ++k) {
      stream << "work[" << k << "] = " << work_[k].first.data() << endl;
    }
  }

  void MXFunctionInternal::allocTape(std::vector<std::pair<std::pair<int, int>, MX> >& tape) {
    // Marker of elements in the work vector still in use when being overwritten
    vector<bool> in_use(work_.size(), false);

    // Remove existing entries in the tape
    tape.clear();

    // Evaluate the algorithm, keeping track of variables that are in use
    int alg_counter = 0;
    for (vector<AlgEl>::iterator it=algorithm_.begin(); it!=algorithm_.end(); ++it, ++alg_counter) {
      if (it->op!=OP_OUTPUT) {
        // Loop over operation outputs, spill if necessary
        for (int c=0; c<it->res.size(); ++c) {
          int ind = it->res[c];
          if (ind>=0) {
            if (in_use[ind]) {
              // Spill
              tape.push_back(make_pair(make_pair(alg_counter, ind), MX()));
            } else {
              // Mark in use
              in_use[ind] = true;
            }
          }
        }
      }
    }
  }

  void MXFunctionInternal::generateDeclarations(std::ostream &stream, const std::string& type,
                                                CodeGenerator& gen) const {

    // Make sure that there are no free variables
    if (!free_vars_.empty()) {
      casadi_error("Code generation is not possible since variables "
                   << free_vars_ << " are free.");
    }

    // Add sparsity patterns in the intermediate variables
    for (int i=0; i<work_.size(); ++i) {
      gen.addSparsity(work_[i].first.sparsity());
    }

    // Generate code for the embedded functions
    for (vector<AlgEl>::const_iterator it=algorithm_.begin(); it!=algorithm_.end(); ++it) {
      switch (it->op) {
      case OP_CALL:
      case OP_SOLVE:
        gen.addDependency(it->data->getFunction());
        break;
      default:
        break;
      }
    }
  }

  void MXFunctionInternal::generateBody(std::ostream &stream, const std::string& type,
                                        CodeGenerator& gen) const {

    // Data structure to hold intermediate variables
    stream << "  static struct wstruct {" << endl;

    // Declare all work variables
    for (int i=0; i<work_.size(); ++i) {
      stream << "    d a" << i << "[" << work_[i].first.size() << "];" << endl;
    }

    // Finalize work structure
    stream << "  } w;" << endl;
    stream << endl;

    // Temporary variables and vectors
    stream << "  int i, j, k, *ii, *jj, *kk;" << endl;
    stream << "  d r, s, t, *rr, *ss, *tt;" << endl;
    stream << "  static int iii[" << itmp_.size() << "];" << endl;
    stream << "  static d rrr[" << rtmp_.size() << "];" << endl;

    // Operation number (for printing)
    int k=0;

    // Names of operation argument and results
    vector<string> arg, res;

    // Print class (for debugging)
    bool codegen_class = true;

    // Codegen the algorithm
    for (vector<AlgEl>::const_iterator it=algorithm_.begin(); it!=algorithm_.end(); ++it) {
      // Mark the beginning of the operation
      stream << "  /* " << k++;
      if (codegen_class) {
        if (it->data.get()!=0) {
          stream << " : " << typeid(*it->data.get()).name();

          // if this is a call node, also write the name of the Function
          MX algElem = it->data;
          if (algElem.getOp() == OP_CALL) {
            stream << " (" << algElem.getFunction().getSanitizedName() << ")";
          }
        }
      }
      stream << " */" << endl;

      // Get the names of the operation arguments
      arg.resize(it->arg.size());
      if (it->op == OP_INPUT) {
        arg.front() = "x" + CodeGenerator::numToString(it->arg.front());
      } else {
        for (int i=0; i<it->arg.size(); ++i) {
          if (it->arg.at(i)>=0) {
            arg.at(i) = "w.a" + CodeGenerator::numToString(it->arg.at(i));
          } else {
            arg.at(i) = "0";
          }
        }
      }

      // Get the names of the operation results
      res.resize(it->res.size());
      if (it->op == OP_OUTPUT) {
        res.front() = "r" + CodeGenerator::numToString(it->res.front());
      } else {
        for (int i=0; i<it->res.size(); ++i) {
          if (it->res.at(i)>=0) {
            res.at(i) = "w.a" + CodeGenerator::numToString(it->res.at(i));
          } else {
            res.at(i) = "0";
          }
        }
      }

      // Print the operation
      if (it->op==OP_OUTPUT) {
        gen.copyVector(stream, arg.front(), output(it->res.front()).size(), res.front(), "i", true);
      } else if (it->op==OP_INPUT) {
        gen.copyVector(stream, arg.front(), input(it->arg.front()).size(), res.front(), "i", false);
      } else {
        it->data->generateOperation(stream, arg, res, gen);
      }
    }
  }

  void MXFunctionInternal::generateLiftingFunctions(MXFunction& vdef_fcn, MXFunction& vinit_fcn) {
    assertInit();

    vector<MX> swork(work_.size());

    MXPtrV input_p, output_p;
    MXPtrVV dummy_p;

    // Definition of intermediate variables
    vector<MX> y;
    vector<MX> g;
    vector<MX> f_G(getNumOutputs());

    // Initial guess for intermediate variables
    vector<MX> x_init;

    // Temporary stringstream
    stringstream ss;

    for (int algNo=0; algNo<2; ++algNo) {
      for (vector<AlgEl>::iterator it=algorithm_.begin(); it!=algorithm_.end(); ++it) {
        switch (it->op) {
        case OP_LIFT:
          {
            MX& arg = swork[it->arg.at(0)];
            MX& arg_init = swork[it->arg.at(1)];
            MX& res = swork[it->res.front()];
            switch (algNo) {
            case 0:
              ss.str(string());
              ss << "y" << y.size();
              y.push_back(MX::sym(ss.str(), arg.sparsity()));
              g.push_back(arg);
              res = y.back();
              break;
            case 1:
              x_init.push_back(arg_init);
              res = arg_init;
              break;
            }
            break;
          }
        case OP_INPUT:
        case OP_PARAMETER:
          swork[it->res.front()] = it->data;
          break;
        case OP_OUTPUT:
          if (algNo==0) {
            f_G[it->res.front()] = swork[it->arg.front()];
          }
          break;
        default:
          {
            input_p.resize(it->arg.size());
            for (int i=0; i<input_p.size(); ++i) {
              int el = it->arg[i];
              input_p[i] = el<0 ? 0 : &swork[el];
            }

            output_p.resize(it->res.size());
            for (int i=0; i<output_p.size(); ++i) {
              int el = it->res[i];
              output_p[i] = el<0 ? 0 : &swork[el];
            }

            it->data->evaluateMX(input_p, output_p, dummy_p, dummy_p, dummy_p, dummy_p, false);
          }
        }
      }
    }

    // Definition of intermediate variables
    vector<MX> f_in = inputv_;
    f_in.insert(f_in.end(), y.begin(), y.end());
    vector<MX> f_out = f_G;
    f_out.insert(f_out.end(), g.begin(), g.end());
    vdef_fcn = MXFunction(f_in, f_out);
    vdef_fcn.setOption("name", "lifting_variable_definition");

    // Initial guess of intermediate variables
    f_in = inputv_;
    f_out = x_init;
    vinit_fcn = MXFunction(f_in, f_out);
    vinit_fcn.setOption("name", "lifting_variable_guess");
  }

} // namespace casadi

