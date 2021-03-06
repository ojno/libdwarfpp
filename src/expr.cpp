/* dwarfpp: C++ binding for a useful subset of libdwarf, plus extra goodies.
 * 
 * expr.cpp: basic C++ abstraction of DWARF expressions.
 *
 * Copyright (c) 2010, Stephen Kell.
 */

#include <limits>
#include <map>
#include <set>
#include <srk31/endian.hpp>

#include "lib.hpp"
#include "expr.hpp" 

using std::map;
using std::pair;
using std::make_pair;
using std::set;
using std::string;
using std::cerr;
using std::endl;
using dwarf::spec::opt;

namespace dwarf
{
	namespace lib
	{
		evaluator::evaluator(const encap::loclist& loclist,
			Dwarf_Addr vaddr,
			const ::dwarf::spec::abstract_def& spec,
			regs *p_regs,
			boost::optional<Dwarf_Signed> frame_base,
			const std::stack<Dwarf_Unsigned>& initial_stack)
		: m_stack(initial_stack), spec(spec), p_regs(p_regs), tos_is_value(false), frame_base(frame_base)
		{
			// sanity check while I suspect stack corruption
			assert(vaddr < 0x00008000000000ULL
			|| 	vaddr == 0xffffffffULL
			||  vaddr == 0xffffffffffffffffULL);
			
			i = expr.begin();
			Dwarf_Addr current_vaddr_base = 0; // relative to CU "applicable base" (Dwarf 3 sec 3.1)
			/* Search through loc expressions for the one that matches vaddr. */
			for (auto i_loc_expr = loclist.begin();
					i_loc_expr != loclist.end();
					++i_loc_expr)
			{
				/* HACK: we should instead use address_size as reported by next_cu_header,
				 * lifting it to a get_address_size() method in spec::compile_unit_die. */
				// Dwarf_Addr magic_addr = 
					
				if (i_loc_expr->lopc == 0xffffffffU
				||  i_loc_expr->lopc == 0xffffffffffffffffULL)
				{
					/* This is a "base address selection entry". */
					current_vaddr_base = i_loc_expr->hipc;
					continue;
				}
				
				/* According to the libdwarf manual, 
				 * lopc == 0 and hipc == 0 means "for all vaddrs".
				 * I seem to have been using 
				 * 0..std::numeric_limits<Dwarf_Addr>::max() for this.
				 * For now, allow both. */
				
				if ((i_loc_expr->lopc == 0 && // this kind of loc_expr covers all vaddrs
					i_loc_expr->hipc == std::numeric_limits<Dwarf_Addr>::max())
				|| (i_loc_expr->lopc == 0 && i_loc_expr->hipc == 0)
				|| (vaddr >= i_loc_expr->lopc + current_vaddr_base
					&& vaddr < i_loc_expr->hipc + current_vaddr_base))
				{
					expr = *i_loc_expr/*->m_expr*/;
					i = expr.begin();
					eval();
					return;
				}
			}
			
			/* Dump something about the vaddr. */
			cerr << "Vaddr 0x" << std::hex << vaddr << std::dec
				<< " is not covered by any loc expr in " << loclist << endl;
			throw No_entry();
		}
	}
	namespace encap
	{
		std::ostream& operator<<(std::ostream& s, const ::dwarf::encap::loclist& ll)
		{
			s << "(loclist) {";
			for (::dwarf::encap::loclist::const_iterator i = ll.begin(); i != ll.end(); i++)
			{
				if (i != ll.begin()) s << ", ";
				s << *i;
			}
			s << "}";
			return s;
		}
		std::ostream& operator<<(std::ostream& s, const loc_expr& e)
		{
			s << "loc described by { ";
			for (std::vector<Dwarf_Loc>::const_iterator i = e./*m_expr.*/begin(); i != e./*m_expr.*/end(); i++)
			{
				s << *i << " ";
			}
			s << "} (for ";
			if (e.lopc == 0 && e.hipc == std::numeric_limits<Dwarf_Addr>::max()) // FIXME
			{
				s << "all vaddrs";
			}
			else
			{
				s << "vaddr 0x"
					<< std::hex << e.lopc << std::dec
					<< "..0x"
					<< std::hex << e.hipc << std::dec;
			}
			s << ")";
			return s;
		}
		loc_expr::loc_expr(Dwarf_Debug dbg, lib::Dwarf_Ptr instrs, lib::Dwarf_Unsigned len, const spec::abstract_def& spec /* = spec::dwarf3 */)
			: spec(spec)
		{
			auto loc = core::Locdesc::try_construct(dbg, instrs, len);
			assert(loc);
			core::Locdesc ld(std::move(loc));
			*static_cast<vector<expr_instr> *>(this) = vector<expr_instr>(ld.raw_handle()->ld_s, ld.raw_handle()->ld_s + ld.raw_handle()->ld_cents);
			this->hipc = ld.raw_handle()->ld_hipc;
			this->lopc = ld.raw_handle()->ld_lopc;
		}			
		
        std::vector<std::pair<loc_expr, Dwarf_Unsigned> > loc_expr::pieces() const
        {
            /* Split the loc_expr into pieces, and return pairs
             * of the subexpr and the length of the object segment
             * that this expression locates (or 0 for "whole object"). 
             * Note that pieces may
             * not be nested (DWARF 3 Sec. 2.6.4, read carefully). */
            std::vector<std::pair<loc_expr, Dwarf_Unsigned> > ps;

            std::vector<expr_instr>::const_iterator done_up_to_here = /*m_expr.*/begin();
            Dwarf_Unsigned done_this_many_bytes = 0UL;
            for (auto i_instr = /*m_expr.*/begin(); i_instr != /*m_expr.*/end(); ++i_instr)
            {
                if (i_instr->lr_atom == DW_OP_piece) 
                {
                    ps.push_back(std::make_pair(
                    	loc_expr(/*std::vector<expr_instr>(*/done_up_to_here, i_instr/*)*/, spec),
	                    i_instr->lr_number));
    	            done_this_many_bytes += i_instr->lr_number;
        	        done_up_to_here = i_instr + 1;
                }
                assert(i_instr->lr_atom != DW_OP_bit_piece); // FIXME: not done bit_piece yet
            }
            // if we did any DW_OP_pieces, we should have finished on one
            assert(done_up_to_here == /*m_expr.*/end() || done_this_many_bytes == 0UL);
            // if we didn't finish on one, we need to add a singleton to the pieces vector
            if (done_this_many_bytes == 0UL) ps.push_back(
                std::make_pair(
                    	loc_expr(*this), 0));
            return ps;
        }
        loc_expr loc_expr::piece_for_offset(Dwarf_Off offset) const
        {
        	auto ps = pieces();
            Dwarf_Off cur_off = 0UL;
            for (auto i = ps.begin(); i != ps.end(); i++)
            {
            	if (i->second == 0UL) return i->first;
                else if (offset >= cur_off && offset < cur_off + i->second) return i->first;
                else cur_off += i->second;
            }
            throw No_entry(); // no piece covers that offset -- likely a bogus offset
        }
		loclist::loclist(const dwarf::lib::loclist& dll)
		{
			for (int i = 0; i != dll.len(); i++)
			{
				push_back(loc_expr(dll[i])); 
			}
		}
		loclist::loclist(const core::LocList& ll)
		{
			for (auto i = ll.copied_list.begin(); i != ll.copied_list.end(); ++i)
			{
				push_back(*i->get());
			}
		}
		loclist::loclist(const core::Locdesc& l)
		{
			push_back(*l.handle.get());
		}
		rangelist::rangelist(const core::RangeList& rl)
		{
			for (unsigned i = 0; i < rl.handle.get_deleter().len; ++i)
			{
				push_back(rl.handle.get()[i]);
			}
		}
		
		loc_expr loclist::loc_for_vaddr(Dwarf_Addr vaddr) const
        {
        	for (auto i = this->begin(); i != this->end(); i++)
            {
            	if (vaddr >= i->lopc && vaddr < i->hipc) return *i;
            }
            throw No_entry(); // bogus vaddr
        }
		
		// FIXME: what was the point of this method? It's some kind of normalisation
		// so that everything takes the form of adding to a pre-pushed base address. 
		// But why? Who needs it?
		loclist absolute_loclist_to_additive_loclist(const loclist& l)
		{
			/* Total HACK, for now: just rewrite DW_OP_fbreg to { DW_OP_consts(n), DW_OP_plus },
			 * i.e. assume the stack pointer is already pushed. */
			loclist new_ll = l;
			for (auto i_l = new_ll.begin(); i_l != new_ll.end(); ++i_l)
			{
				for (auto i_instr = i_l->begin(); i_instr != i_l->end(); ++i_instr)
				{
					if (i_instr->lr_atom == DW_OP_fbreg)
					{
						i_instr->lr_atom = DW_OP_consts; // leave argument the same
						// skip i_instr along one (might now be at end())
						++i_instr;
						// insert the plus, and leave i_instr pointing at it
						i_instr = i_l->insert(i_instr, (Dwarf_Loc) { .lr_atom = DW_OP_plus });
					}
				}
			}
			return new_ll;
		}
// 		boost::icl::interval_map<Dwarf_Addr, vector<expr_instr> > 
// 		loclist::as_interval_map() const
// 		{
// 			boost::icl::interval_map<Dwarf_Addr, vector<expr_instr> > working;
// 			for (auto i_loc_expr = begin(); i_loc_expr != end(); ++i_loc_expr)
// 			{
// 				auto interval = boost::icl::discrete_interval<Dwarf_Addr>::right_open(
// 						i_loc_expr->lopc,
// 						i_loc_expr->hipc
// 					);
// 				working += make_pair(
// 					interval,
// 					*i_loc_expr
// 				);
// 			}
// 			return working;
// 		}
	}
}
