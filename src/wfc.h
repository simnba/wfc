#pragma once

#include "grid.h"

#include <iostream>
#include <fstream>
#include <iomanip>

#include <vector>
#include <set>
#include <map>
#include <queue>
#include <stack>
#include <cassert>
#include <chrono>
#include <algorithm>
#include <numeric>
#include <iterator>
#include <mutex>
#include <filesystem>
#include <random>


namespace wfc
{
	template<typename T>
	class uniqueStack {
		std::stack<T> st;
		std::set<T> se;
		mutable std::mutex mtx;
	public:
		bool push(T i) {
			//AutoTimer at(g_timer, _FUNC_);
			const std::lock_guard lock(mtx);
			if (se.contains(i))
				return false;
			st.push(i);
			se.insert(i);
			return true;
		}
		bool empty() { return st.empty(); }
		T pop() {
			auto temp = st.top();
			st.pop();
			se.erase(temp);
			return temp;
		}
		int size() const {
			assert(st.size() == se.size());
			return st.size();
		}
	};



	// ---------------------------------------------------------------------------------------
	// The segments

	struct option {
		int modIdx;
		short rot;
		bool mirrorX;
		int prio = 0;
		bool operator==(option const&)const = default;
	};

	using option_id = short;
	using superposition = std::vector<option_id>;
	using wavefunction = std::vector<superposition>;

	class SegmentPalette {
	public:
		virtual int getNumOptions() const = 0;
		virtual option const& getOption(option_id i) const = 0;
		virtual bool isPossibleNeighbour(superposition const& sp, int dir, option const& otherOpt, int otherDir) const = 0;
	};

	// ---------------------------------------------------------------------------------------
	// The possibilities

	template<class grid_t>
	struct gridState{
		using face_t = typename grid_t::face_t;
		const grid_t* grid = nullptr;
		const SegmentPalette* palette = nullptr;
		wavefunction wave;
		superposition allSP;

		struct faceInfo{
			bool dirty = false;
		};
		std::vector<faceInfo> faceInfos;

		void init(grid_t const* g, SegmentPalette const* p){
			grid = g;
			palette = p;

			allSP.resize(palette->getNumOptions());
			std::iota(std::begin(allSP), std::end(allSP), 0);

			// Creates the total superposition for each cell in the grid:
			wave.resize(g->faces.size(), allSP);
			faceInfos.resize(g->faces.size());
		}

		face_id getLowestEntropyFace() const {
			AutoTimer at(g_timer, _FUNC_);

//			int i;
//			do{(i = rand() % grid->faces.size());}
//			while(this->getOptions(i).size() <= 1);
//			return i;

			std::vector<face_id> minPosis; int minEntropy = std::numeric_limits<int>::max();
			grid->forAllFaces([&, this](face_t const& c) {
				face_id id = &c-grid->faces.data();
				const superposition& sp = this->getOptions(id);
				if (sp.size() > 1) {
					if (sp.size() < minEntropy) minPosis = { id }, minEntropy = sp.size();
					else if (sp.size() == minEntropy) minPosis.push_back(id);
				}
			});
			return minPosis[rand() % minPosis.size()];
		}

		const face_id getNeighbourFace(face_id f, int d) const {
			return grid->faces[f].neighbours[d];
		}
		auto& getOptions(this auto&& self, face_id f) {
			return self.wave[f];
		}

		auto& getInfo(this auto&& self, face_id f) {
			return self.faceInfos[f];
		}

		bool isCollapsed() const {
			for (auto& a : wave) if (a.size() == 0) return true; else if (a.size() > 1) return false;
			return true;
		}
		bool isStuck() const {
			for (auto& a : wave) if (a.size() == 0) return true;
			return false;
		}
		bool isAnyMirrored() const {
			for (auto& a : wave)
				for (auto& b : a)
					if (palette->getOption(b).mirrorX)
						return true;
			return false;
		}

		void printCanvas(std::ostream& os, std::string sep = "") const {
			for (auto& sp : wave) {
				if (sp.size() != 1)
					os << "-1 0 0" << sep;
				else {
					const option& opt = palette->getOption(sp[0]);
					os << opt.modIdx << " " << opt.rot << " " << opt.mirrorX << sep;
				}
				os << std::endl;
			}
		}
	};



	// ---------------------------------------------------------------------------------------
	// The algorithm

	template<class grid_t>
	class WaveFunctionCollapser
	{
		using face_t = typename grid_t::face_t;
		using state_t = typename gridState<grid_t>;
		state_t* state;
		SegmentPalette const* palette;
		std::mt19937 rg;


		// Fills the superposition of the slot based on hard conditions. The result is used as the initial state for wfc.
		void fillOptions(face_id f) {
			state->getOptions(f) = state->allSP;
		}

		void collapse(face_id c) {
			superposition& sp = state->getOptions(c);
			sp = { sp[rand() % sp.size()] };
		}

		// Remove options of neighbouring cells that do not fit to any option of the given cell. Returns if the neighbours have options left.
		bool propagate(face_id c) {
			AutoTimer at(g_timer, _FUNC_);
			std::map<face_id, superposition> backup;
			uniqueStack<face_id> jobs;
			const auto& faces = state->grid->faces;
			jobs.push(c);
			while (!jobs.empty()) {
				face_id p = jobs.pop();
				superposition& sp = state->getOptions(p);
				for (int d = 0; d < faces[p].getNumCorners(); ++d) {
					face_id nnp = state->getNeighbourFace(p, d);
					if (nnp != -1) {
						superposition& nnsp = state->getOptions(nnp);
						if (nnsp.size() <= 1) continue;
						if (!backup.contains(nnp)) backup[nnp] = nnsp;

						int dstar = state->grid->getComplFace(p, d);

						erase_neighbours:
						{
							//AutoTimer at(g_timer, "erase neighbours");
							if (std::erase_if(nnsp, [&](int const& otherOpt) {
								return !palette->isPossibleNeighbour(sp, d, palette->getOption(otherOpt), dstar);
							}))
								jobs.push(nnp);
						}

						if (nnsp.empty()) {
							if (auto fi = state->getInfo(nnp); !fi.dirty) {
								// we reached a dead end but there is hope: we can re-update this slot and we might get new options:
								fillOptions(nnp);
								fi.dirty = true;
								if (nnsp != backup[nnp]) {
									backup[nnp] = nnsp;
									goto erase_neighbours;
								}
							} else goto undo_propagate;
						}
					}
				}
			}
			return true;

			undo_propagate:
			{
				AutoTimer at(g_timer, "undo propagate");
				for (const auto& p: backup)
					state->getOptions(p.first) = p.second;
			}
			return false;
		}

		// Fills superpositions of all (touched) slots with the prefiltered options.
		void fillWave(bool all){
			state->grid->forAllFaces([&, counter=0](face_t const& c)mutable{
				bool needsUpdate = state->getInfo(&c).dirty || all;
				if(needsUpdate){// suppose this is a new slot
					fillOptions(c);
				}
			});
		}

		// Returns a list of numbers from 0 to n-1 in random order but such that the prio(ret[i]) >= prio(ret[i+1]).
		std::vector<int> sort_shuffle(int n, std::function<int(int)> prio, std::mt19937& rg){
			if(n==1)return {0};
			std::vector<int> rand_order(n);
			std::iota(std::begin(rand_order), std::end(rand_order), 0);
			std::shuffle(std::begin(rand_order), std::end(rand_order), rg);
			std::sort(std::begin(rand_order), std::end(rand_order), [&](int i, int j){return prio(i)>prio(j);});
			return rand_order;
		}

	public:
		void solve(state_t* s) {
			AutoTimer at(g_timer, _FUNC_);

			state = s;
			palette = s->palette;

			while (!state->isCollapsed()) {
				auto p = state->getLowestEntropyFace();
				collapse(p);
				propagate(p);
			}

			std::cout << "isAnyMirrored = " << state->isAnyMirrored() << std::endl;
			std::cout << "isStuck = " << state->isStuck() << std::endl;
		}


		// Returns if successfull i.e. not stuck.
		bool backtrack(int& out_nrecursions, int& out_niters, int stage) {
			std::string ph = std::string(stage, '|');
			if (state->isCollapsed())
				return !state->isStuck();
			out_nrecursions++;
			const auto& c = state->getLowestEntropyFace();
			superposition& sp = state->getOptions(&c);
			//	std::cerr<<"getLowestEntropyPos = "<<p->mycell<< " ori "<<p->ori<<endl;
			if (sp.size() == 1)
				state->getInfo(&c).dirty = false;
			superposition backup = sp;
			//std::cerr << ph << "backup="; for (int i : backup) std::cerr << i << ", ";
			//std::cerr << std::endl;
			std::vector<int> rand_order = sort_shuffle(backup.size(),
													   [&](int i) {return palette->getOption(i).prio; }, rg);
			for (int idx : rand_order) {
				// Make a choice:
				int a = backup[idx];
				sp = { a };
				//std::cerr << ph << "choose " << a << std::endl;
				if (!propagate(c))
				{
					//std::cerr << ph << "redone choice " << a << std::endl;
					continue;
				}
				//std::cerr << ph << "recursion ->" << std::endl;
				if (!backtrack(out_nrecursions, out_niters, stage + 1))
				{
					//std::cerr << ph << "backtrace " << a << std::endl;
					continue;
				}
				//std::cerr << ph << "success" << std::endl;
				return true;
			}
			sp = backup;
			return false;
		}



		// Solves the wave with backtracking.
		void solveRecursive(state_t* s) { //, newModuleCB_t&& newMod
			AutoTimer at(g_timer, _FUNC_);

			state = s;
			palette = s->palette;

			fillWave(true);
			auto action = [&]() mutable {

//				// Map the intern wave function to the grid slots:
//				wavefunction gridwave(gs..getNumCells() * 8);
//				g.forAllFaces([&, counter = 0](Cell&, int, slot& s)mutable{
//					gridwave[counter++] = &s.sp;
//				});

				// Collapse and propagate:
				auto t0 = std::chrono::high_resolution_clock::now();
				int nrecursions = 0, niters = 0;

				backtrack(nrecursions, niters, 0);

				std::cout << "Wave function collapse done:" << std::endl;
				std::cout << "> Time = " << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - t0).count() << " ms" << std::endl;
				std::cout << "> Wave size = " << state->wave.size() << std::endl;
				std::cout << "> recursion = " << nrecursions << std::endl;
				std::cout << "> neighbour-interactions = " << niters << std::endl;
				std::cout << "> isAnyMirrored = " << state->isAnyMirrored() << std::endl;
				std::cout << "> isStuck = " << state->isStuck() << std::endl;

				//realize(g, newMod);
			};

			action();
		}
	};

}
