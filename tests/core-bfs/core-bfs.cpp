#include <fstream>
#include <fileno.hpp>
#include <dwarfpp/lib.hpp>
#include <dwarfpp/attr.hpp>

using std::cout; 
using std::endl;
using namespace dwarf;

int static_we_should_find;

int main(int argc, char **argv)
{
	cout << "Opening " << argv[0] << "..." << endl;
	std::ifstream in(argv[0]);
	core::root_die root(fileno(in));

	cout << "Searching for variables..." << endl;
	core::iterator_bf<> i = root.begin();
	assert(i != root.end());
	int prev_depth = 0;
	for (; i != root.end(); ++i)
	{
		int cur_depth = i.depth();
		assert(cur_depth >= prev_depth);
		if (cur_depth != prev_depth)
		{
			cout << "Now at depth "  << cur_depth << endl;
		}
		prev_depth = cur_depth;
		
		// cerr << "Saw " << i << endl;
		if (i.tag_here() == DW_TAG_variable
			&& i.has_attribute_here(DW_AT_location))
		{
			/* DWARF doesn't tell us whether a variable is static or not. 
			 * We want to rule out non-static variables. To do this, we
			 * rely on our existing lib:: infrastructure. */
			core::Attribute a(dynamic_cast<core::Die&>(i.get_handle()), DW_AT_location);
			encap::attribute_value val(a, dynamic_cast<core::Die&>(i.get_handle()), root);
			auto loclist = val.get_loclist();
			bool reads_register = false;
			for (auto i_loc_expr = loclist.begin(); 
				i_loc_expr != loclist.end(); 
				++i_loc_expr)
			{
				for (auto i_instr = i_loc_expr->begin(); 
					i_instr != i_loc_expr->end();
					++i_instr)
				{
					if (spec::DEFAULT_DWARF_SPEC.op_reads_register(i_instr->lr_atom))
					{
						reads_register = true;
						break;
					}
				}
				if (reads_register) break;
			}
			if (!reads_register)
			{
				auto name = i.name_here();
				cout << "Found a static or global variable named "
					<< (name ? name->c_str() : "(no name)") << endl;
			}
			else
			{
				auto name = i.name_here();
				cout << "Found a local variable named "
					<< (name ? name->c_str() : "(no name)") << endl;
			}
		} 
	}

	
	return 0;
}
