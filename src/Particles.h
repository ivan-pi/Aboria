/*

Copyright (c) 2005-2016, University of Oxford.
All rights reserved.

University of Oxford means the Chancellor, Masters and Scholars of the
University of Oxford, having an administrative office at Wellington
Square, Oxford OX1 2JD, UK.

This file is part of Aboria.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
 * Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.
 * Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.
 * Neither the name of the University of Oxford nor the names of its
   contributors may be used to endorse or promote products derived from this
   software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/


#ifndef PARTICLES_H_
#define PARTICLES_H_

#include <vector>
#include <random>
#include <string>
//#include <boost/array.hpp>
//#include <boost/iterator/iterator_facade.hpp>
#include <boost/iterator/counting_iterator.hpp>
#include <boost/range/adaptors.hpp>

#include "Zip.h"
#include "Get.h"
#include "Vector.h"
#include "Variable.h"
#include "Traits.h"
#include "BucketSearchSerial.h"
//#include "OctTree.h"
#include "CudaInclude.h"

#ifdef HAVE_VTK
#include <vtkUnstructuredGrid.h>
#include <vtkSmartPointer.h>
#include <vtkIntArray.h>
#include <vtkFloatArray.h>
#include <vtkPointData.h>
#include <vtkCellArray.h>
#include <vtkUnsignedCharArray.h>
#endif

#include "detail/Particles.h"


namespace Aboria {
namespace detail {
template <unsigned int D, typename Reference>
struct enforce_domain_impl {
    typedef Vector<double,D> double_d;
    typedef Vector<bool,D> bool_d;
    typedef position_d<D> position;
    static const unsigned int dimension = D;
    const double_d low,high;
    const bool_d periodic;

    enforce_domain_impl(const double_d &low, const double_d &high, const bool_d &periodic):
            low(low),high(high),periodic(periodic) {}

    CUDA_HOST_DEVICE
    void operator()(Reference i) const {
        double_d r = Aboria::get<position>(i);
        for (unsigned int d = 0; d < dimension; ++d) {
            if (periodic[d]) {
                while (r[d]<low[d]) {
                    r[d] += (high[d]-low[d]);
                }
                while (r[d]>=high[d]) {
                    r[d] -= (high[d]-low[d]);
                }
            } else {
                if ((r[d]<low[d]) || (r[d]>=high[d])) {
#ifdef __CUDA_ARCH__
                    LOG_CUDA(2,"removing particle");
#else
                    LOG(2,"removing particle with r = "<<r);
#endif
                    Aboria::get<alive>(i) = uint8_t(false);
                }
            }
        }
        Aboria::get<position>(i) = r;
    }
};

template <typename Reference>
struct resize_lambda {
    uint32_t seed;
    int next_id;
    const size_t *start_id_pointer;

    resize_lambda(const uint32_t &seed, const int &next_id, const size_t *start_id_pointer):
            seed(seed),next_id(next_id),start_id_pointer(start_id_pointer) {}

    CUDA_HOST_DEVICE
    void operator()(Reference i) const {
        Aboria::get<alive>(i) = true;

        const size_t index = &Aboria::get<id>(i)-start_id_pointer;
        Aboria::get<id>(i) = index + next_id;

        generator_type& gen = Aboria::get<generator>(i);
        gen.seed(seed + uint32_t(Aboria::get<id>(i)));

    }
};

template <typename Reference>
struct set_seed_lambda {
    uint32_t seed;

    set_seed_lambda(const uint32_t &seed):
            seed(seed) {}

    CUDA_HOST_DEVICE
    void operator()(Reference i) const {
        generator_type& gen = Aboria::get<generator>(i);
        gen.seed(seed + uint32_t(Aboria::get<id>(i)));
    }
};

template <typename ConstReference>
struct is_alive {
    CUDA_HOST_DEVICE
    bool operator()(ConstReference i) const {
        return Aboria::get<alive>(i);
    }
};



}
    /*
#ifdef HAVE_THUST
template <typename T, typename Alloc> 
using default_vector = thrust::host_vector<T,Alloc>;
#else
template <typename T, typename Alloc> 
using default_vector = std::vector<T,Alloc>;
#endif
*/

/// \brief A STL-compatable container of particles in 3D space
///
///  Each particle has a 3D position and user-defined data-package 
///  (for other variables such as velocity, density etc) and is 
///  optionally embedded within a cuboidal spatial domain (for neighbourhood searches) 
///  that can be periodic or not. Each particle also has its own random number 
///  generator that is seeded via its own unique id.
///
///  For example, the following creates a set of particles which each have 
///  (along with the standard variables such as position, id etc) a 
///  data package consisting of one double variable type named scalar.
///
///  \code 
///     using namespace Aboria;
///     ABORIA_VARIABLE(scalar,double,"my scalar")
///     typedef Particles<scalar> MyParticles;
///     MyParticles particles();
///  \endcode
///
///  \param TYPES a list of one or more variable types
///
///  \see #ABORIA_VARIABLE
template<typename VAR=std::tuple<>, unsigned int D=3, template <typename,typename> class VECTOR=std::vector, template <typename> class SearchMethod=bucket_search_serial, typename TRAITS_USER=Traits<VECTOR> > 
class Particles {
public:

    ///
    /// This type
    typedef Particles<VAR,D,VECTOR,SearchMethod,TRAITS_USER> particles_type;

    ///
    /// The traits type used to build up the Particle container.
    /// Contains Level 0 vector class and dimension information
    typedef TraitsCommon<VAR,D,TRAITS_USER> traits_type;

    /// 
    /// a tuple type containing value_types for each Variable
    typedef typename traits_type::value_type value_type;

    /// 
    /// a tuple type containing references to value_types for each Variable
    typedef typename traits_type::reference reference;

    /// 
    /// a tuple type containing const_references to value_types for each Variable
    typedef typename traits_type::const_reference const_reference;

    /// 
    /// a tuple type containing raw references to value_types for each Variable
    typedef typename traits_type::raw_pointer raw_pointer;

    /// 
    /// a tuple type containing raw references to value_types for each Variable
    typedef typename traits_type::raw_reference raw_reference;

    /// 
    /// a tuple type containing raw references to value_types for each Variable
    typedef typename traits_type::raw_const_reference raw_const_reference;
   
    /// 
    /// type used to hold data (a tuple of vectors)
    typedef typename traits_type::data_type data_type;

    /// 
    /// type used to hold and return size information
    typedef typename traits_type::size_type size_type;

    /// 
    /// type used to hold and return the difference of iterator, or distance between
    typedef typename traits_type::size_type difference_type;

    /// 
    /// non-const iterator type
    /// \see zip_iterator
    typedef typename traits_type::iterator iterator;

    /// 
    /// const iterator type
    /// \see zip_iterator
    typedef typename traits_type::const_iterator const_iterator;

    ///
    /// the neighbourhood data structure that the particles will be embedded into
    typedef SearchMethod<traits_type> search_type;

    ///
    /// the query class that is associated with search_type
    typedef typename search_type::query_type query_type;

    /// a boost mpl vector type containing a vector of Variable 
    /// attached to the particles (includes position, id and 
    /// alive flag as well as all user-supplied variables)
    typedef typename traits_type::mpl_type_vector mpl_type_vector;

    template <typename T>
    using elem_by_type = detail::get_elem_by_type<T,mpl_type_vector>;
    template <typename T>
    using return_type = typename detail::getter_helper<typename data_type::tuple_type>::template return_type<elem_by_type<T>::index>;

    ///
    /// the number of spatial dimensions 
    static const unsigned int dimension = D;

    ///
    /// a type to store a vector of doubles with given dimension
    typedef Vector<double,dimension> double_d;

    ///
    /// a type to store a vector of doubles with given dimension
    typedef Vector<double,dimension> int_d;

    ///
    /// a type to store a vector of bool with given dimension
    typedef Vector<bool,dimension> bool_d;

    ///
    /// the tag type for the default position variable
    typedef typename traits_type::position position;


    /// Contructs an empty container with no searching or id tracking enabled
    Particles():
        next_id(0),
        searchable(false),
        seed(time(NULL))
    {}

    /// Constructs a container with `size` particles
    Particles(const size_t size):
        next_id(0),
        searchable(false),
        seed(time(NULL))
    {
        resize(size);
    }

    /// copy-constructor. performs deep copying of all particles
    Particles(const particles_type &other):
            data(other.data),
            search(other.search),
            next_id(other.next_id),
            searchable(other.searchable),
            seed(other.seed)
    {}

    /// range-based copy-constructor. performs deep copying of all 
    /// particles from \p first to \p last
    Particles(iterator first, iterator last):
        data(traits_type::construct(first,last)),
        searchable(false),
        seed(0)
    {}

    
    //
    // STL Container
    //
    

    /// resize the continer. Note that if new particles are created, they
    /// are NOT added to the neighbour search structure, and might be outside
    /// the domain
    void resize(size_type n) {
        size_t old_n = this->size();
        traits_type::resize(data,n);         
        if (n > old_n) {
            const size_t *start_id_pointer = 
                iterator_to_raw_pointer(get<id>(data).begin() + old_n); 
            detail::for_each(begin()+old_n, end(), 
                detail::resize_lambda<raw_reference>(seed,next_id,start_id_pointer));
            next_id += n-old_n;
        }
    }
    
    /// push the particle \p val to the back of the container (if its within
    /// the searchable domain)
    void push_back (const value_type& val, bool update_neighbour_search=true) {
        // add val to container
        traits_type::push_back(data,val);

        // overwrite id, alive and random generator
        reference i = *(end()-1);
        Aboria::get<id>(i) = this->next_id++;
        Aboria::get<generator>(i) = generator_type((seed + uint32_t(Aboria::get<id>(i))));
        Aboria::get<alive>(i) = true;

        if (search.domain_has_been_set()) {
            detail::enforce_domain_impl<traits_type::dimension,reference> enforcer(search.get_min(),search.get_max(),search.get_periodic());
            enforcer(i);
        }
        if (get<alive>(i)) {
            if (search.domain_has_been_set() && update_neighbour_search) {
                if (search.add_points_at_end(begin(),end()-1,end())) {
                    reorder(search.get_order().begin(),search.get_order().end());
                }
            }
        } else {
            LOG(2,"WARNING: particle you tried to push back with r = "<<
                    static_cast<const double_d&>(Aboria::get<position>(i)) <<
                    " is outside the domain and has been removed");
            pop_back(false);
            search.update_iterators(begin(),end());
        }

    }

    /// set the base seed of the container. Note that the random number generator for
    /// each particle is set to \p value plus the particle's id
    void set_seed(const uint32_t value) {
        seed = value;
        detail::for_each(begin(),end(),
                detail::set_seed_lambda<raw_reference>(seed));
        
    }

    /// push a new particle with position \p position
    /// to the back of the container
    void push_back(const double_d& pos) {
        value_type i;
        Aboria::get<position>(i) = pos;
        this->push_back(i);
    }

    /// push the particles in \p particles to the back of the container
    void push_back (const particles_type& particles) {
        for (const value_type& i: particles) {
            this->push_back(i,false);
        }
        if (searchable) {
            if (search.add_points_at_end(data.begin(),data.end()-particles.size(),data.end())) {
                reorder(search.get_order().begin(),search.get_order().end());
            }
        }
    }

    /// pop (delete) the particle at the end of the container 
    void pop_back(bool update_neighbour_search=true) {
        erase(end()-1,update_neighbour_search);
    }

    /// returns a reference to the particle at position \p idx
    reference operator[](std::size_t idx) {
        return traits_type::index(data, idx);
    }

    /// returns a const_reference to the particle at position \p idx
    const_reference operator[](std::size_t idx) const {
        return traits_type::index(data, idx);
    }

    /*
    /// returns a const reference to the particle at position \p idx
    const_value_type operator[](std::size_t idx) const {
        return traits_type::index_const(data, idx);
    }
    */

    /// returns an iterator to the beginning of the container
    iterator begin() {
        return traits_type::begin(data);
    }

    /// returns an iterator to the end of the container
    iterator end() {
        return traits_type::end(data);
    }

    /// returns an iterator to the beginning of the container
    const_iterator begin() const {
        return traits_type::cbegin(data);
    }

    /// returns an iterator to the end of the container
    const_iterator end() const {
        return traits_type::cend(data);
    }

    /// returns a const_iterator to the beginning of the container
    const_iterator cbegin() const {
        return traits_type::cbegin(data);
    }

    /// returns an iterator to the end of the container
    const_iterator cend() const {
        return traits_type::cend(data);
    }

    /// sets container to empty and deletes all particles
    void clear() {
        return traits_type::clear(data);
    }

    /// erase the particle pointed to by the iterator \p i.
    /// NOTE: This will potentially reorder the particles
    /// if neighbourhood searching is on, then this is updated
    iterator erase (iterator i, const bool update_neighbour_search = true) {
        const size_t i_position = i-begin();

        if (update_neighbour_search) {
            search.before_delete_particles_range(i_position,1);
        }

        if (i_position == size()-1) {
            // just pop off back element
            traits_type::pop_back(data);
            i = end();
        } else {
            if (search.ordered()) {
                // have to maintain order and move everthing...
                traits_type::erase(data,i);
            } else {
                // copy end element to i and pop off end element
                *i = *(end()-1);
                traits_type::pop_back(data);
            }
        }
            
        if (update_neighbour_search) {
            if (search.after_delete_particles_range(begin(),end(),i_position,1)) {
                reorder(search.get_order().begin(),search.get_order().end());
            }
        }
        return i;
    }


    /// erase the particles between the iterators \p first
    /// and \p last
    /// \see erase(iterator)
    iterator erase (iterator first, iterator last, const bool update_neighbour_search = true) {
        iterator return_iterator = last;

        const size_t n_before_range = first-begin();
        const size_t n = last-first;
        const size_t n_after_range = end()-last;

        if (update_neighbour_search) {
            search.before_delete_particles_range(n_before_range,n);
        }

        if (n_after_range > n && !search.ordered()) {
            // move elements at end to deleted region
            detail::copy(end()-n,end(),first);
            traits_type::resize(data,size()-n);         
        } else {
            // order is maintained
            traits_type::erase(data,first,last);
        }
        
        if (update_neighbour_search) {
            if (search.after_delete_particles_range(begin(),end(),n_before_range,n)) {
                reorder(search.get_order().begin(),search.get_order().end());
            }
        }
        
        return end()-n_after_range; 
    }

    /// insert a particle \p val into the container at \p position
    iterator insert (iterator position, const value_type& val) {
        return traits_type::insert(data,position,val);
    }

    /// insert a \p n copies of the particle \p val into the container at \p position
    void insert (iterator position, size_type n, const value_type& val) {
        traits_type::insert(data,position,n,val);
    }

    /// insert a range of particles pointed to by \p first and \p last at \p position 
    template <class InputIterator>
    iterator insert (iterator position, InputIterator first, InputIterator last) {
        return insert_dispatch(position, first, last, std::integral_constant<bool,
                            std::is_same<InputIterator,iterator>::value 
                            || std::is_same<InputIterator,const_iterator>::value>());
    }

    /// return the total number of particles in the container
    size_t size() const {
        return Aboria::get<position>(data).size();
    }


    //
    // Neighbourhood Searching
    //

    /// initialise the neighbourhood searching for the particle container.
    /// The neighbourhood searching is performed over a cuboidal domain from 
    /// \p low to \p high.
    ///
    /// \param low the lowest point in the search domain
    /// \param high the highest point in the search domain
    /// \param length_scale the maximum search radius required. particles closer 
    /// to each other than this length are considered neighbours
    /// \param periodic a boolean 3d vector indicating whether each dimension 
    /// is periodic (true) or not (false)
    void init_neighbour_search(const double_d& low, const double_d& high, const bool_d& periodic,
                                const unsigned int n_particles_in_leaf=10) {
        LOG(2, "Particles:init_neighbour_search: low = "<<low<<" high = "<<high<<" periodic = "<<periodic<<" n_particles_in_leaf = "<<n_particles_in_leaf);

        search.set_domain(low,high,periodic,n_particles_in_leaf);

        //enforce_domain(search.get_min(),search.get_max(),search.get_periodic());
        
        detail::for_each(begin(), end(),
                detail::enforce_domain_impl<traits_type::dimension,reference>(low,high,periodic));

        if ((periodic == false).any()) {
            // delete particles outside domain
            iterator first_dead = detail::partition(begin(),end(),
                    detail::is_alive<raw_const_reference>());
            traits_type::erase(data,first_dead,end());
        }

        if (search.add_points_at_end(begin(),begin(),end())) {
            reorder(search.get_order().begin(),search.get_order().end());
        }

        searchable = true;
    }

    void init_id_search() {
        LOG(2, "Particles:init_id_search");
        search.update_iterators(begin(),end());
        search.init_id_map();
        searchable = true;
    }

    const query_type& get_query() const {
        ASSERT(searchable,"init_neighbour_search not called on this particle set");
        return search.get_query();
    }


    double_d correct_dx_for_periodicity(const double_d& uncorrected_dx) const {
        double_d dx = uncorrected_dx;
        //double_d domain_width = get_max()-get_min();
        const bool_d& periodic = get_periodic();
        for (size_t d=0; d<traits_type::dimension; ++d) {
            if (periodic[d]) {
                const double domain_width = get_max()[d]-get_min()[d];
                while (dx[d] > domain_width/2) dx[d] -= domain_width;
                while (dx[d] <= -domain_width/2) dx[d] += domain_width;
            }
        }
        return dx;
    }

    /// return the length scale of the neighbourhood search
    /// \see init_neighbour_search()
    double_d get_lengthscale() const {
        return search.get_min_bucket_size();
    }

    /// return the lower extent of the neighbourhood search
    /// \see init_neighbour_search()
    const double_d& get_min() const {
        return search.get_min();
    }

    /// return the upper extent of the neighbourhood search
    /// \see init_neighbour_search()
    const double_d& get_max() const {
        return search.get_max();
    }

    /// return the periodicty of the neighbourhood search
    /// \see init_neighbour_search()
    const bool_d& get_periodic() const {
        return search.get_periodic();
    }

    /// Update the neighbourhood search data. This function must be
    /// called after altering the particle positions (e.g. with 
    /// `set<position>(particle,new_position)`) in order for accurate
    /// neighbourhood searching
    /// \see get_neighbours()
    void update_positions(const bool assume_all_alive=false) {
        if (search.domain_has_been_set()) {

            detail::for_each(begin(), end(),
                detail::enforce_domain_impl<traits_type::dimension,reference>(
                    search.get_min(),search.get_max(),search.get_periodic()));

            int num_dead = 0;
            if (!assume_all_alive)  {
                auto fnot = [](const bool in) { return static_cast<int>(!in); };
                m_delete_indicies.resize(size());
                detail::inclusive_scan(detail::make_transform_iterator(get<alive>(cbegin()),fnot),
                        detail::make_transform_iterator(get<alive>(cend()),fnot),
                        m_delete_indicies.begin());
                num_dead = *(m_delete_indicies.end()-1);
            }

            // if any dead, or search imposes an order, then will request a reorder
            if (search.update_positions(num_dead,m_delete_indicies)) {
                reorder(search.get_order().begin(),search.get_order().end());

                // after this point all dead particles will be at the end
                traits_type::erase(data,end()-num_dead,end());
            }




            /*
            if ((search.get_periodic()==false).any()) {
                // delete particles and update positions
                delete_particles(true);
            } else {
                // just update positions
                search.update_positions();
            }
            */
        }
    }

    //
    // Particle Creation/Deletion
    //
    
    /// deletes all particles with alive==false from the container
    /// NOTE: this will reorder the particles in the container, invalidating
    /// any iterators
    /// \param update_neighbour_search updates neighbourhood search
    /// information if true (default=true)
    void delete_particles(const bool update_positions=false) {
        LOG(2,"Particle: delete_particles: update_positions= "<<update_positions);

        // if we don't need to update positions, and are running in serial,
        // and the neighbour search isn't ordered, then
        // do a serial update using copy_points
        /*
        const bool do_serial_delete = search.domain_has_been_set() && 
                                      !update_positions &&
                                      detail::concurrent_processes<traits_type>()==1 && 
                                      !search.ordered();
                                      */
        const size_t old_size = size();
        

        /*
         * don't think I need this anymore
        if (do_serial_delete) {
            for (int index = 0; index < size(); ++index) {
                iterator i = begin() + index;
                while (Aboria::get<alive>(*i) == false) {
                    LOG(3,"Particle: delete_particles: deleting particle "<<
                            get<id>(*i)<<" with position "<<
                            static_cast<const double_d&>(get<position>(*i)));
                    search.copy_points(end()-1,i);
                    if ((index < size()-1) && (size() > 1)) {
                        *i = *(end()-1);
                        pop_back(false);
                        i = begin() + index;
                    } else {
                        pop_back(false);
                        break;
                    }
                }
                LOG(4,"Particle: delete_particles: now iterator has particle "<<
                        get<id>(*i)<<" with position "<<
                        static_cast<const double_d&>(get<position>(*i))<<
                        " and alive "<< bool(get<alive>(*i)));
            }
        } else {


        if (update_positions) {
            traits_type::erase(data,
                    detail::partition(get<alive>(begin()),get<alive>(end())),
                    end());

            if (search.after_delete_particles(begin(),end(),update_positions)) {
                reorder(search.get_order().begin(),search.get_order().end());
            }
        } else {

        auto fnot = [](const bool in) { return static_cast<int>(!in); };
        m_delete_indicies.resize(size());
        detail::inclusive_scan(detail::make_transform_iterator(get<alive>(cbegin()),fnot),
                               detail::make_transform_iterator(get<alive>(cend()),fnot),
                               m_delete_indicies.begin());
        const int num_dead = *(m_delete_indicies.end()-1);

        // this allows the search ds to see which paricles will be deleted

        if (num_dead > 0) {
            search.before_delete_particles(m_delete_indicies);


            //TODO: if num_dead > threshold do a partion copy?
            if (search.ordered() || num_dead > threshold) {

                raw_pointer& begin_this  = begin();
                raw_pointer& begin_other = iterator_to_raw_pointer(
                                            traits_type::begin(other_data));
                detail::for_each(count(),count()+n,
                        [&](int i) {
                            if (!get<alive>(begin_this)[i]) {
                                begin_other[m_delete_indicies[i]] = begin_this[i];
                            }
                        });
                
                data.swap(other_data);
            } else {
                detail::for_each(begin(),end(),search.get_copy_points_lambda());
            }
            
            traits_type::erase(data,end()-num_dead,end());

            if (search.after_delete_particles(begin(),end(),update_positions)) {
                reorder(search.get_order().begin(),search.get_order().end());
            }
        }
        }
        */

    }

    // Need to be mark as device to enable get functions being device/host
    CUDA_HOST_DEVICE
    const typename data_type::tuple_type & get_tuple() const { 
        return data.get_tuple(); 
    }

    // Need to be mark as device to enable get functions being device/host
    CUDA_HOST_DEVICE
    typename data_type::tuple_type & get_tuple() { 
        return data.get_tuple(); 
    }

    /*
private:
    struct set_variable {
        set_variable(data_type &data_to_set, data_type &internal_data, int n):
            data_to_set(data_to_set),internal_data(internal_data),n(n) {}
        template< typename U > void operator()(U i) {
            typedef typename mpl::at<mpl_type_vector,U>::type variable_type;
            const char *name = variable_type().name;
            CHECK(n == data_to_set.get<U::value>().size(), "error, data vectors not the same size")
            internal_data.set<U::value>(data_to_set.get<U::value>());
        }

        data_type &data_to_set;
        data_type &internal_data;
        int n;
    };
public:

    void set(data_type &data_to_set) {
        mpl::for_each<mpl::range_c<int,1,mpl::size<mpl_type_vector>::type::value> > (set_variable(data_to_set,data,data_to_set.get<0>().size()));
    }
    */

    friend std::ostream& operator<< (std::ostream& stream, const Particles& particles) {
        traits_type::header_to_stream(stream);
        stream << '\n';
        for (const_iterator i=particles.cbegin(); i!=particles.cend(); ++i) { 
            traits_type::to_stream(i,stream);
            stream << '\n';
        }
        return stream;
    }

    friend std::istream& operator>> (std::istream& stream, Particles& particles) {
        stream.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        for (iterator i=particles.begin(); i!=particles.end(); ++i) { 
            traits_type::from_stream(i);
        }
        return stream;
    }

    template<class Archive>
    void serialize(Archive & ar, const unsigned int version) {
        traits_type::serialize(data,ar,version);
    }


#ifdef HAVE_VTK
    
    /// get a vtk unstructured grid version of the particle container
    /// This grid is cached internally. The first time this function is 
    /// called the grid is created and updated with the particle data.
    /// If the particle data is updated subsequently you need to set \p refresh=true
    /// for the grid to be updated
    vtkSmartPointer<vtkUnstructuredGrid> get_grid(bool refresh = false) {
        if (!cache_grid) {
            cache_grid = vtkSmartPointer<vtkUnstructuredGrid>::New();
            copy_to_vtk_grid(cache_grid);
        } else if (refresh == true) {
            copy_to_vtk_grid(cache_grid);
        }
        return cache_grid;
    }

    ///  copy the particle data to a VTK unstructured grid
    void  copy_to_vtk_grid(vtkUnstructuredGrid *grid) {
        vtkSmartPointer<vtkPoints> points = grid->GetPoints();
        if (!points) {
            points = vtkSmartPointer<vtkPoints>::New();
            grid->SetPoints(points);
        }
        vtkSmartPointer<vtkCellArray> cells = grid->GetCells();
        if (!cells) {
            cells = vtkSmartPointer<vtkCellArray>::New();
            grid->SetCells(1,cells);
        }
        vtkSmartPointer<vtkUnsignedCharArray> cell_types = grid->GetCellTypesArray();
        

        const vtkIdType n = size();

        constexpr size_t dn = mpl::size<mpl_type_vector>::type::value;
        vtkSmartPointer<vtkFloatArray> datas[dn];
        mpl::for_each<mpl::range_c<int,1,dn> > (
                detail::setup_datas_for_writing<reference>(n,datas,grid)
                );
        points->SetNumberOfPoints(n);
        cells->Reset();
        cell_types->Reset();
        int j = 0;

        double write_point[3];
        const unsigned int max_d = std::min(3u,D);
        for(auto i: *this) {
            const int index = j++;
            //std::cout <<"copying point at "<<i.get_position()<<" with id = "<<i.get_id()<<std::endl;
            const double_d &r = get<position>(i);
            for (int d=0; d<max_d; ++d) {
                write_point[d] = r[d];
            }
            points->SetPoint(index,write_point);
            cells->InsertNextCell(1);
            cells->InsertCellPoint(index);
            cell_types->InsertNextTuple1(1);

            mpl::for_each<mpl::range_c<int,1,dn> > (
                    detail::write_from_tuple<reference>(
                        i.get_tuple(),
                        index,
                        datas,
                        seed + uint32_t(Aboria::get<id>(i))
                        )
                    );
        }
    }


    ///  update the particle data according to a supplied VTK unstructured grid
    ///  it is assumed that \p grid has been generated from copy_to_vtk_grid() or
    ///  get_grid()
    ///  \see get_grid()
    ///  \see copy_to_vtk_grid()
    void  copy_from_vtk_grid(vtkUnstructuredGrid *grid) {
            vtkSmartPointer<vtkPoints> points = grid->GetPoints();
            CHECK(points,"No points in vtkUnstructuredGrid");
            vtkSmartPointer<vtkCellArray> cells = grid->GetCells();
            CHECK(points,"No cells in vtkUnstructuredGrid");
            vtkSmartPointer<vtkUnsignedCharArray> cell_types = grid->GetCellTypesArray();
            constexpr size_t dn = mpl::size<mpl_type_vector>::type::value;

            vtkSmartPointer<vtkFloatArray> datas[dn];

            const vtkIdType n = points->GetNumberOfPoints();

            mpl::for_each<mpl::range_c<int,1,dn> > (
                    detail::setup_datas_for_reading<reference>(n,datas,grid)
                    );

            this->clear();

            const unsigned int max_d = std::min(3u,traits_type::dimension);
            for (int j = 0; j < n; ++j) {
                value_type particle;
                const double *r = points->GetPoint(j);
                for (int d=0; d<max_d; ++d) {
                    get<position>(particle)[d] = r[d];
                }
                mpl::for_each<mpl::range_c<int,1,dn> > (
                        detail::read_into_tuple<reference>(particle.get_tuple(),j,datas)
                        );
                this->push_back(particle);
            }
        }
#endif

private:
    typedef typename traits_type::vector_unsigned_int vector_unsigned_int;
    typedef typename traits_type::vector_int vector_int;


    // sort the particles by order (i.e. a gather)
    void reorder(const typename vector_int::const_iterator& order_begin, 
                 const typename vector_int::const_iterator& order_end) {
        LOG(2,"reordering particles");
        const size_t n = order_end-order_begin;
        if (n > 0) {
            traits_type::resize(other_data,n);         
            detail::gather(order_begin,order_end,
                           traits_type::begin(data),
                           traits_type::begin(other_data));
            data.swap(other_data);
            search.update_iterators(begin(),end());
        }
    }

    template <class InputIterator>
    iterator insert_dispatch (iterator position, InputIterator first, InputIterator last, 
            std::false_type) {
        //TODO: this is very inefficient, but need to handle normal iterators
        //to value_type. Could improve....
        if (first==last) return position;
        for (InputIterator it = last; it != first; --it) {
            position = insert(position, *it);
        }
        return insert(position, *first);
    }

    template <class InputIterator>
    iterator insert_dispatch (iterator position, InputIterator first, InputIterator last, 
            std::true_type) {
        return traits_type::insert(data,position,first,last);
    }


    data_type data;
    data_type other_data;
    int next_id;
    bool searchable;
    uint32_t seed;
    search_type search;
    vector_int m_delete_indicies;


#ifdef HAVE_VTK
    vtkSmartPointer<vtkUnstructuredGrid> cache_grid;
#endif
};



}


#endif /* SPECIES_H_ */
