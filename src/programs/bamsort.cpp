/**
    bambam
    Copyright (C) 2009-2013 German Tischler
    Copyright (C) 2011-2013 Genome Research Limited

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
**/
#include "config.h"

#include <iostream>
#include <queue>

#include <libmaus2/aio/OutputStreamInstance.hpp>

#include <libmaus2/bambam/BamAlignment.hpp>
#include <libmaus2/bambam/BamAlignmentNameComparator.hpp>
#include <libmaus2/bambam/BamAlignmentPosComparator.hpp>
#include <libmaus2/bambam/BamAlignmentHashComparator.hpp>
#include <libmaus2/bambam/BamBlockWriterBaseFactory.hpp>
#include <libmaus2/bambam/BamDecoder.hpp>
#include <libmaus2/bambam/BamEntryContainer.hpp>
#include <libmaus2/bambam/BamMultiAlignmentDecoderFactory.hpp>
#include <libmaus2/bambam/BamStreamingMarkDuplicates.hpp>
#include <libmaus2/bambam/BamWriter.hpp>
#include <libmaus2/bambam/ProgramHeaderLineSet.hpp>

#include <libmaus2/util/ArgInfo.hpp>
#include <libmaus2/util/GetObject.hpp>
#include <libmaus2/util/PutObject.hpp>
#include <libmaus2/util/TempFileRemovalContainer.hpp>

#include <libmaus2/lz/BgzfDeflateOutputCallbackMD5.hpp>
#include <libmaus2/bambam/BgzfDeflateOutputCallbackBamIndex.hpp>
static int getDefaultMD5() { return 0; }
static int getDefaultIndex() { return 0; }

#include <biobambam2/BamBamConfig.hpp>

#if defined(BIOBAMBAM_LIBMAUS2_HAVE_IO_LIB)
#include <libmaus2/bambam/ScramDecoder.hpp>
#endif

#include <biobambam2/Licensing.hpp>

static int getDefaultLevel() { return Z_DEFAULT_COMPRESSION; }
static int getDefaultVerbose() { return 1; }
static std::string getDefaultSortOrder() { return "coordinate"; }
static uint64_t getDefaultBlockSize() { return 1024; }
static bool getDefaultDisableValidation() { return false; }
static std::string getDefaultInputFormat() { return "bam"; }
static int getDefaultFixMates() { return 0; }
static int getDefaultSortThreads() { return 1; }
static int getDefaultCalMdNm() { return 0; }
static int getDefaultCalMdNmRecompIndetOnly() { return 0; }
static int getDefaultCalMdNmWarnChange() { return 0; }
static int getDefaultAddDupMarkSupport() { return 0; }
static int getDefaultMarkDuplicates() { return 0; }
static int getDefaultStreaming() { return 1; }
static int getDefaultRmDup() { return 0; }

/*
   biobambam used MC as a mate coordinate tag which now has a clash
   with the official SAM format spec.  New biobambam version uses mc,
   this function removes the older tag where necessary,
*/

void removeOldStyleMateCoordinate(
	libmaus2::bambam::BamAlignment & rec1,
	libmaus2::bambam::BamAlignment & rec2
	)
{
    	uint64_t num;

    	bool r1 = rec1.getAuxAsNumber<uint64_t>("MC", num);
    	bool r2 = rec2.getAuxAsNumber<uint64_t>("MC", num);

	// old MC is a number type, spec format is a string

	libmaus2::bambam::BamAuxFilterVector OldMCfilter;
	OldMCfilter.set("MC");

	if (r1)
	{
    	    	rec1.filterOutAux(OldMCfilter);
	}

	if (r2)
	{
	    	rec2.filterOutAux(OldMCfilter);
	}
}



int bamsort(::libmaus2::util::ArgInfo const & arginfo)
{
	::libmaus2::util::TempFileRemovalContainer::setup();

	bool const inputisstdin = (!arginfo.hasArg("I")) || (arginfo.getUnparsedValue("I","-") == "-");
	bool const outputisstdout = (!arginfo.hasArg("O")) || (arginfo.getUnparsedValue("O","-") == "-");

	if ( isatty(STDIN_FILENO) && inputisstdin && (arginfo.getValue<std::string>("inputformat","bam") != "sam") )
	{
		::libmaus2::exception::LibMausException se;
		se.getStream() << "Refusing to read binary data from terminal, please redirect standard input to pipe or file." << std::endl;
		se.finish();
		throw se;
	}

	if ( isatty(STDOUT_FILENO) && outputisstdout && (arginfo.getValue<std::string>("outputformat","bam") != "sam") )
	{
		::libmaus2::exception::LibMausException se;
		se.getStream() << "Refusing write binary data to terminal, please redirect standard output to pipe or file." << std::endl;
		se.finish();
		throw se;
	}

	int const verbose = arginfo.getValue<int>("verbose",getDefaultVerbose());
	bool const disablevalidation = arginfo.getValue<int>("disablevalidation",getDefaultDisableValidation());
	bool markduplicates = arginfo.getValue<int>("markduplicates",getDefaultMarkDuplicates());

	std::string const inputformat = arginfo.getUnparsedValue("inputformat",getDefaultInputFormat());

	// prefix for tmp files
	std::string const tmpfilenamebase = arginfo.getValue<std::string>("tmpfile",arginfo.getDefaultTmpFileName());
	std::string const tmpfilenameout = tmpfilenamebase + "_bamsort";
	::libmaus2::util::TempFileRemovalContainer::addTempFile(tmpfilenameout);
	uint64_t blockmem = arginfo.getValue<uint64_t>("blockmb",getDefaultBlockSize())*1024*1024;
	std::string const sortorder = arginfo.getValue<std::string>("SO","coordinate");
	uint64_t sortthreads = arginfo.getValue<uint64_t>("sortthreads",getDefaultSortThreads());
	bool const streaming = arginfo.getValue<unsigned int>("streaming",getDefaultStreaming());

	// input decoder wrapper
	libmaus2::bambam::BamAlignmentDecoderWrapper::unique_ptr_type decwrapper(
		libmaus2::bambam::BamMultiAlignmentDecoderFactory::construct(
			arginfo,false, // do not put rank
			0, /* copy stream */
			std::cin, /* standard input */
			true, /* concatenate instead of merging */
			streaming /* streaming */
		)
	);
	::libmaus2::bambam::BamAlignmentDecoder * ppdec = &(decwrapper->getDecoder());
	::libmaus2::bambam::BamAlignmentDecoder & dec = *ppdec;
	if ( disablevalidation )
		dec.disableValidation();
	::libmaus2::bambam::BamHeader const & header = dec.getHeader();

	std::string const headertext(header.text);

	// add PG line to header
	std::string const upheadtext = ::libmaus2::bambam::ProgramHeaderLineSet::addProgramLine(
		headertext,
		"bamsort", // ID
		"bamsort", // PN
		arginfo.commandline, // CL
		::libmaus2::bambam::ProgramHeaderLineSet(headertext).getLastIdInChain(), // PP
		std::string(PACKAGE_VERSION) // VN
	);
	// construct new header
	::libmaus2::bambam::BamHeader uphead(upheadtext);

	/*
	 * start index/md5 callbacks
	 */
	std::string const tmpfileindex = tmpfilenamebase + "_index";
	::libmaus2::util::TempFileRemovalContainer::addTempFile(tmpfileindex);

	std::string md5filename;
	std::string indexfilename;

	std::vector< ::libmaus2::lz::BgzfDeflateOutputCallback * > cbs;
	::libmaus2::lz::BgzfDeflateOutputCallbackMD5::unique_ptr_type Pmd5cb;
	if ( arginfo.getValue<unsigned int>("md5",getDefaultMD5()) )
	{
		if ( libmaus2::bambam::BamBlockWriterBaseFactory::getMD5FileName(arginfo) != std::string() )
			md5filename = libmaus2::bambam::BamBlockWriterBaseFactory::getMD5FileName(arginfo);
		else
			std::cerr << "[V] no filename for md5 given, not creating hash" << std::endl;

		if ( md5filename.size() )
		{
			::libmaus2::lz::BgzfDeflateOutputCallbackMD5::unique_ptr_type Tmd5cb(new ::libmaus2::lz::BgzfDeflateOutputCallbackMD5);
			Pmd5cb = UNIQUE_PTR_MOVE(Tmd5cb);
			cbs.push_back(Pmd5cb.get());
		}
	}
	libmaus2::bambam::BgzfDeflateOutputCallbackBamIndex::unique_ptr_type Pindex;
	if ( arginfo.getValue<unsigned int>("index",getDefaultIndex()) )
	{
		if ( libmaus2::bambam::BamBlockWriterBaseFactory::getIndexFileName(arginfo) != std::string() )
			indexfilename = libmaus2::bambam::BamBlockWriterBaseFactory::getIndexFileName(arginfo);
		else
			std::cerr << "[V] no filename for index given, not creating index" << std::endl;

		if ( indexfilename.size() )
		{
			libmaus2::bambam::BgzfDeflateOutputCallbackBamIndex::unique_ptr_type Tindex(new libmaus2::bambam::BgzfDeflateOutputCallbackBamIndex(tmpfileindex));
			Pindex = UNIQUE_PTR_MOVE(Tindex);
			cbs.push_back(Pindex.get());
		}
	}
	std::vector< ::libmaus2::lz::BgzfDeflateOutputCallback * > * Pcbs = 0;
	if ( cbs.size() )
		Pcbs = &cbs;
	/*
	 * end md5/index callbacks
	 */
	enum sort_order_type { sort_order_coordinate, sort_order_queryname, sort_order_hash };
	sort_order_type sort_order;

	if ( sortorder == "queryname" )
	{
		uphead.changeSortOrder("queryname");
		sort_order = sort_order_queryname;
	}
	else if ( sortorder == "hash" )
	{
		uphead.changeSortOrder("unknown");
		sort_order = sort_order_hash;
	}
	else
	{
		uphead.changeSortOrder("coordinate");
		sort_order = sort_order_coordinate;
	}

	bool const havetag = arginfo.hasArg("tag");
	std::string const tag = arginfo.getUnparsedValue("tag","no tag");

	if ( havetag && (tag.size() != 2 || (!isalpha(tag[0])) || (!isalnum(tag[1])) ) )
	{
		::libmaus2::exception::LibMausException se;
		se.getStream() << "tag " << tag << " is invalid" << std::endl;
		se.finish();
		throw se;
	}

	// nucl tag field
	bool const havenucltag = arginfo.hasArg("nucltag");
	std::string const nucltag = arginfo.getUnparsedValue("nucltag","no tag");

	if ( havenucltag && (nucltag.size() != 2 || (!isalpha(nucltag[0])) || (!isalnum(nucltag[1])) ) )
	{
		::libmaus2::exception::LibMausException se;
		se.getStream() << "nucltag " << tag << " is invalid" << std::endl;
		se.finish();
		throw se;
	}

	if ( havetag && havenucltag )
	{
		::libmaus2::exception::LibMausException se;
		se.getStream() << "tag and nucltag are mutually exclusive" << std::endl;
		se.finish();
		throw se;
	}

	enum tag_type_enum
	{
		tag_type_none,
		tag_type_string,
		tag_type_nucleotide
	};
	tag_type_enum tag_type;

	if ( havetag )
		tag_type = tag_type_string;
	else if ( havenucltag )
		tag_type = tag_type_nucleotide;
	else
		tag_type = tag_type_none;

	bool addMSMC = arginfo.getValue<int>("adddupmarksupport",getDefaultAddDupMarkSupport());
	bool fixmates = arginfo.getValue<int>("fixmates",getDefaultFixMates());
	bool rmdup = arginfo.getValue<int>("rmdup",getDefaultRmDup());

	if ( rmdup && (!markduplicates) )
	{
		std::cerr << "[W] rmdup is enabled, forcing markduplicates=1" << std::endl;
		markduplicates = true;
	}

	if ( (havetag || havenucltag) && (!addMSMC) )
	{
		std::cerr << "[W] tag or nucltag is enabled, forcing adddupmarksupport=1" << std::endl;
		addMSMC = true;
	}

	if ( markduplicates && (!addMSMC) )
	{
		std::cerr << "[W] markduplicates is enabled, forcing adddupmarksupport=1" << std::endl;
		addMSMC = true;
	}

	if ( addMSMC && ! fixmates )
	{
		std::cerr << "[W] adddupmarksupport is enabled, forcing fixmates=1" << std::endl;
		fixmates = true;
	}

	libmaus2::bambam::BamBlockWriterBase::unique_ptr_type Uout ( libmaus2::bambam::BamBlockWriterBaseFactory::construct(uphead, arginfo, Pcbs) );
	libmaus2::bambam::BamBlockWriterBase * Pout = Uout.get();
	libmaus2::bambam::BamStreamingMarkDuplicates::unique_ptr_type MaDuout;

	if ( markduplicates )
	{
		libmaus2::bambam::BamStreamingMarkDuplicates::unique_ptr_type TMaDuout(
			new libmaus2::bambam::BamStreamingMarkDuplicates(arginfo,header,*Pout,true /* filter tags out */, true /* put rank */,
				libmaus2::bambam::BamStreamingMarkDuplicates::getDefaultFilterOld(),
				rmdup
			)
		);
		MaDuout = UNIQUE_PTR_MOVE(TMaDuout);
		Pout = MaDuout.get();
	}

	libmaus2::bambam::BamBlockWriterBase & alout = *Pout;
	libmaus2::autoarray::AutoArray<char> MCaux;

	if ( fixmates )
	{
		if ( sort_order == sort_order_coordinate )
		{
			::libmaus2::bambam::BamEntryContainer< ::libmaus2::bambam::BamAlignmentPosComparator >
				BEC(blockmem,tmpfilenameout,sortthreads);

			if ( verbose )
				std::cerr << "[V] Reading alignments from source." << std::endl;
			uint64_t incnt = 0;

			// current alignment
			libmaus2::bambam::BamAlignment & curalgn = dec.getAlignment();
			// previous alignment
			libmaus2::bambam::BamAlignment prevalgn;
			// previous alignment valid
			bool prevalgnvalid = false;
			// MQ field filter
			libmaus2::bambam::BamAuxFilterVector MQfilter;
			libmaus2::bambam::BamAuxFilterVector MSfilter;
			libmaus2::bambam::BamAuxFilterVector MCfilter;
			libmaus2::bambam::BamAuxFilterVector MTfilter;
			libmaus2::bambam::BamAuxFilterVector CMCfilter;

			MQfilter.set("MQ");
			MSfilter.set("ms");
			MCfilter.set("mc");
			MTfilter.set("mt");
			CMCfilter.set("MC");

			// remove the original style tags (MC handled separately)
			MSfilter.set("MS");
			MTfilter.set("MT");

			while ( dec.readAlignment() )
			{
				if ( curalgn.isSecondary() || curalgn.isSupplementary() )
				{
					BEC.putAlignment(curalgn);
				}
				else if ( prevalgnvalid )
				{
					// different name
					if ( strcmp(curalgn.getName(),prevalgn.getName()) )
					{
						BEC.putAlignment(prevalgn);
						curalgn.swap(prevalgn);
					}
					// same name
					else
					{
						libmaus2::bambam::BamAlignment::fixMateInformation(prevalgn,curalgn,MQfilter);

						if ( addMSMC )
						{
							libmaus2::bambam::BamAlignment::addMateBaseScore(prevalgn,curalgn,MSfilter);
							libmaus2::bambam::BamAlignment::addMateCoordinate(prevalgn,curalgn,MCfilter);
							libmaus2::bambam::BamAlignment::addMateCigarString(prevalgn,curalgn,MCaux,CMCfilter);
							removeOldStyleMateCoordinate(prevalgn,curalgn);

							switch ( tag_type )
							{
								case tag_type_string:
									libmaus2::bambam::BamAlignment::addMateTag(prevalgn,curalgn,MTfilter,tag);
									break;
								case tag_type_nucleotide:
									libmaus2::bambam::BamAlignment::addMateTag(prevalgn,curalgn,MTfilter,nucltag);
									break;
								default:
									break;
							}
						}

						BEC.putAlignment(prevalgn);
						BEC.putAlignment(curalgn);
						prevalgnvalid = false;
					}
				}
				else
				{
					prevalgn.swap(curalgn);
					prevalgnvalid = true;
				}

				if ( verbose && ( ( ++incnt & ((1ull<<20)-1) ) == 0 ) )
					std::cerr << "[V] " << incnt << std::endl;
			}

			if ( prevalgnvalid )
			{
				BEC.putAlignment(prevalgn);
				prevalgnvalid = false;
			}

			if ( verbose )
				std::cerr << "[V] read " << incnt << " alignments" << std::endl;

			// BEC.createOutput(std::cout, uphead, level, verbose, Pcbs);
			BEC.createOutput(alout, verbose);
		}
		else if ( sort_order == sort_order_hash )
		{
			::libmaus2::bambam::BamEntryContainer< ::libmaus2::bambam::BamAlignmentHashComparator<> >
				BEC(blockmem,tmpfilenameout,sortthreads);

			if ( verbose )
				std::cerr << "[V] Reading alignments from source." << std::endl;
			uint64_t incnt = 0;

			// current alignment
			libmaus2::bambam::BamAlignment & curalgn = dec.getAlignment();
			// previous alignment
			libmaus2::bambam::BamAlignment prevalgn;
			// previous alignment valid
			bool prevalgnvalid = false;
			// MQ field filter
			libmaus2::bambam::BamAuxFilterVector MQfilter;
			libmaus2::bambam::BamAuxFilterVector MSfilter;
			libmaus2::bambam::BamAuxFilterVector MCfilter;
			libmaus2::bambam::BamAuxFilterVector MTfilter;
			libmaus2::bambam::BamAuxFilterVector CMCfilter;
			MQfilter.set("MQ");
			MSfilter.set("ms");
			MCfilter.set("mc");
			MTfilter.set("mt");
			CMCfilter.set("MC");

			// remove the original style tags (MC handled separately)
			MSfilter.set("MS");
			MTfilter.set("MT");

			while ( dec.readAlignment() )
			{
				if ( curalgn.isSecondary() || curalgn.isSupplementary() )
				{
					BEC.putAlignment(curalgn);
				}
				else if ( prevalgnvalid )
				{
					// different name
					if ( strcmp(curalgn.getName(),prevalgn.getName()) )
					{
						BEC.putAlignment(prevalgn);
						curalgn.swap(prevalgn);
					}
					// same name
					else
					{
						libmaus2::bambam::BamAlignment::fixMateInformation(prevalgn,curalgn,MQfilter);

						if ( addMSMC )
						{
							libmaus2::bambam::BamAlignment::addMateBaseScore(prevalgn,curalgn,MSfilter);
							libmaus2::bambam::BamAlignment::addMateCoordinate(prevalgn,curalgn,MCfilter);
							libmaus2::bambam::BamAlignment::addMateCigarString(prevalgn,curalgn,MCaux,CMCfilter);
    	    	    	    	    	    	    	removeOldStyleMateCoordinate(prevalgn,curalgn);

							switch ( tag_type )
							{
								case tag_type_string:
									libmaus2::bambam::BamAlignment::addMateTag(prevalgn,curalgn,MTfilter,tag);
									break;
								case tag_type_nucleotide:
									libmaus2::bambam::BamAlignment::addMateTag(prevalgn,curalgn,MTfilter,nucltag);
									break;
								default:
									break;
							}
						}

						BEC.putAlignment(prevalgn);
						BEC.putAlignment(curalgn);
						prevalgnvalid = false;
					}
				}
				else
				{
					prevalgn.swap(curalgn);
					prevalgnvalid = true;
				}

				if ( verbose && ( ( ++incnt & ((1ull<<20)-1) ) == 0 ) )
					std::cerr << "[V] " << incnt << std::endl;
			}

			if ( prevalgnvalid )
			{
				BEC.putAlignment(prevalgn);
				prevalgnvalid = false;
			}

			if ( verbose )
				std::cerr << "[V] read " << incnt << " alignments" << std::endl;

			// BEC.createOutput(std::cout, uphead, level, verbose, Pcbs);
			BEC.createOutput(alout, verbose);
		}
		else
		{
			::libmaus2::bambam::BamEntryContainer< ::libmaus2::bambam::BamAlignmentNameComparator >
				BEC(blockmem,tmpfilenameout,sortthreads);

			if ( verbose )
				std::cerr << "[V] Reading alignments from source." << std::endl;
			uint64_t incnt = 0;

			// current alignment
			libmaus2::bambam::BamAlignment & curalgn = dec.getAlignment();
			// previous alignment
			libmaus2::bambam::BamAlignment prevalgn;
			// previous alignment valid
			bool prevalgnvalid = false;
			// MQ field filter
			libmaus2::bambam::BamAuxFilterVector MQfilter;
			libmaus2::bambam::BamAuxFilterVector MSfilter;
			libmaus2::bambam::BamAuxFilterVector MCfilter;
			libmaus2::bambam::BamAuxFilterVector MTfilter;
			libmaus2::bambam::BamAuxFilterVector CMCfilter;
			MQfilter.set("MQ");
			MSfilter.set("ms");
			MCfilter.set("mc");
			MTfilter.set("mt");
			CMCfilter.set("MC");

			// remove the original style tags (MC handled separately)
			MSfilter.set("MS");
			MTfilter.set("MT");

			while ( dec.readAlignment() )
			{
				if ( curalgn.isSecondary() || curalgn.isSupplementary() )
				{
					BEC.putAlignment(curalgn);
				}
				else if ( prevalgnvalid )
				{
					// different name
					if ( strcmp(curalgn.getName(),prevalgn.getName()) )
					{
						BEC.putAlignment(prevalgn);
						curalgn.swap(prevalgn);
					}
					// same name
					else
					{
						libmaus2::bambam::BamAlignment::fixMateInformation(prevalgn,curalgn,MQfilter);

						if ( addMSMC )
						{
							libmaus2::bambam::BamAlignment::addMateBaseScore(prevalgn,curalgn,MSfilter);
							libmaus2::bambam::BamAlignment::addMateCoordinate(prevalgn,curalgn,MCfilter);
							libmaus2::bambam::BamAlignment::addMateCigarString(prevalgn,curalgn,MCaux,CMCfilter);
    	    	    	    	    	    	    	removeOldStyleMateCoordinate(prevalgn,curalgn);

							switch ( tag_type )
							{
								case tag_type_string:
									libmaus2::bambam::BamAlignment::addMateTag(prevalgn,curalgn,MTfilter,tag);
									break;
								case tag_type_nucleotide:
									libmaus2::bambam::BamAlignment::addMateTag(prevalgn,curalgn,MTfilter,nucltag);
									break;
								default:
									break;
							}
						}

						BEC.putAlignment(prevalgn);
						BEC.putAlignment(curalgn);
						prevalgnvalid = false;
					}
				}
				else
				{
					prevalgn.swap(curalgn);
					prevalgnvalid = true;
				}

				if ( verbose && ( ( ++incnt & ((1ull<<20)-1) ) == 0 ) )
					std::cerr << "[V] " << incnt << std::endl;
			}

			if ( prevalgnvalid )
			{
				BEC.putAlignment(prevalgn);
				prevalgnvalid = false;
			}

			if ( verbose )
				std::cerr << "[V] read " << incnt << " alignments" << std::endl;

			// BEC.createOutput(std::cout, uphead, level, verbose, Pcbs);
			BEC.createOutput(alout, verbose);
		}
	}
	else
	{
		if ( sort_order == sort_order_coordinate )
		{
			bool const calmdnm = arginfo.getValue<unsigned int>("calmdnm",getDefaultCalMdNm());
			if ( calmdnm && (! arginfo.hasArg("calmdnmreference")) )
			{
				libmaus2::exception::LibMausException lme;
				lme.getStream() << "calmdnm is set but required calmdnmreference is not, aborting." << std::endl;
				lme.finish();
				throw lme;
			}
			std::string const calmdnmreference = arginfo.getUnparsedValue("calmdnmreference","");
			bool const calmdnmrecompindetonly = arginfo.getValue<unsigned int>("calmdnmrecompindetonly",getDefaultCalMdNmRecompIndetOnly());
			bool const calmdnmwarnchange = arginfo.getValue<unsigned int>("calmdnmwarnchange",getDefaultCalMdNmWarnChange());

			::libmaus2::bambam::BamEntryContainer< ::libmaus2::bambam::BamAlignmentPosComparator > BEC(blockmem,tmpfilenameout,sortthreads);

			if ( verbose )
				std::cerr << "[V] Reading alignments from source." << std::endl;
			uint64_t incnt = 0;

			while ( dec.readAlignment() )
			{
				BEC.putAlignment(dec.getAlignment());
				incnt++;
				if ( verbose && (incnt % (1024*1024) == 0) )
					std::cerr << "[V] " << incnt/(1024*1024) << "M" << std::endl;
			}

			if ( verbose )
				std::cerr << "[V] read " << incnt << " alignments" << std::endl;


			if ( calmdnm )
			{
				libmaus2::bambam::MdNmRecalculation mdnmrecalc(calmdnmreference,false /* do not validate again */,calmdnmrecompindetonly,calmdnmwarnchange,64*1024);
				BEC.createOutput(alout, verbose, &mdnmrecalc);
			}
			else
			{
				BEC.createOutput(alout, verbose, 0);
			}
		}
		else if ( sort_order == sort_order_hash )
		{
			::libmaus2::bambam::BamEntryContainer< ::libmaus2::bambam::BamAlignmentHashComparator<> > BEC(blockmem,tmpfilenameout,sortthreads);

			if ( verbose )
				std::cerr << "[V] Reading alignments from source." << std::endl;
			uint64_t incnt = 0;

			while ( dec.readAlignment() )
			{
				BEC.putAlignment(dec.getAlignment());
				incnt++;
				if ( verbose && (incnt % (1024*1024) == 0) )
					std::cerr << "[V] " << incnt/(1024*1024) << "M" << std::endl;
			}

			if ( verbose )
				std::cerr << "[V] read " << incnt << " alignments" << std::endl;

			// BEC.createOutput(std::cout, uphead, level, verbose, Pcbs);
			BEC.createOutput(alout, verbose);
		}
		else
		{
			::libmaus2::bambam::BamEntryContainer< ::libmaus2::bambam::BamAlignmentNameComparator > BEC(blockmem,tmpfilenameout,sortthreads);

			if ( verbose )
				std::cerr << "[V] Reading alignments from source." << std::endl;
			uint64_t incnt = 0;

			while ( dec.readAlignment() )
			{
				BEC.putAlignment(dec.getAlignment());
				incnt++;
				if ( verbose && (incnt % (1024*1024) == 0) )
					std::cerr << "[V] " << incnt/(1024*1024) << "M" << std::endl;
			}

			if ( verbose )
				std::cerr << "[V] read " << incnt << " alignments" << std::endl;

			// BEC.createOutput(std::cout, uphead, level, verbose, Pcbs);
			BEC.createOutput(alout, verbose);
		}
	}

	// flush duplicate marking if active
	if ( MaDuout )
	{
		MaDuout->flush();
		MaDuout->writeMetrics(arginfo);
		MaDuout.reset();
	}

	// flush encoder so callbacks see all output data
	Uout.reset();

	if ( Pmd5cb )
	{
		Pmd5cb->saveDigestAsFile(md5filename);
	}
	if ( Pindex )
	{
		Pindex->flush(std::string(indexfilename));
	}

	return EXIT_SUCCESS;
}

int main(int argc, char * argv[])
{
	try
	{
		::libmaus2::util::ArgInfo const arginfo(argc,argv);

		for ( uint64_t i = 0; i < arginfo.restargs.size(); ++i )
			if (
				arginfo.restargs[i] == "-v"
				||
				arginfo.restargs[i] == "--version"
			)
			{
				std::cerr << ::biobambam2::Licensing::license();
				return EXIT_SUCCESS;
			}
			else if (
				arginfo.restargs[i] == "-h"
				||
				arginfo.restargs[i] == "--help"
			)
			{
				std::cerr << ::biobambam2::Licensing::license();
				std::cerr << std::endl;
				std::cerr << "Key=Value pairs:" << std::endl;
				std::cerr << std::endl;

				std::vector< std::pair<std::string,std::string> > V;

				V.push_back ( std::pair<std::string,std::string> ( "level=<["+::biobambam2::Licensing::formatNumber(getDefaultLevel())+"]>", libmaus2::bambam::BamBlockWriterBaseFactory::getBamOutputLevelHelpText() ) );
				V.push_back ( std::pair<std::string,std::string> ( "SO=<["+getDefaultSortOrder()+"]>", "sorting order (coordinate, queryname or hash)" ) );
				V.push_back ( std::pair<std::string,std::string> ( "verbose=<["+::biobambam2::Licensing::formatNumber(getDefaultVerbose())+"]>", "print progress report" ) );
				V.push_back ( std::pair<std::string,std::string> ( "blockmb=<["+::biobambam2::Licensing::formatNumber(getDefaultBlockSize())+"]>", "size of internal memory buffer used for sorting in MiB" ) );
				V.push_back ( std::pair<std::string,std::string> ( "disablevalidation=<["+::biobambam2::Licensing::formatNumber(getDefaultDisableValidation())+"]>", "disable input validation (default is 0)" ) );
				V.push_back ( std::pair<std::string,std::string> ( "tmpfile=<filename>", "prefix for temporary files, default: create files in current directory" ) );
				V.push_back ( std::pair<std::string,std::string> ( "md5=<["+::biobambam2::Licensing::formatNumber(getDefaultMD5())+"]>", "create md5 check sum (default: 0)" ) );
				V.push_back ( std::pair<std::string,std::string> ( "md5filename=<filename>", "file name for md5 check sum" ) );
				V.push_back ( std::pair<std::string,std::string> ( "index=<["+::biobambam2::Licensing::formatNumber(getDefaultIndex())+"]>", "create BAM index (default: 0)" ) );
				V.push_back ( std::pair<std::string,std::string> ( "indexfilename=<filename>", "file name for BAM index file" ) );
				V.push_back ( std::pair<std::string,std::string> ( std::string("inputformat=<[")+getDefaultInputFormat()+"]>", std::string("input format (") + libmaus2::bambam::BamMultiAlignmentDecoderFactory::getValidInputFormats() + ")" ) );
				V.push_back ( std::pair<std::string,std::string> ( std::string("outputformat=<[")+libmaus2::bambam::BamBlockWriterBaseFactory::getDefaultOutputFormat()+"]>", std::string("output format (") + libmaus2::bambam::BamBlockWriterBaseFactory::getValidOutputFormats() + ")" ) );
				V.push_back ( std::pair<std::string,std::string> ( "I=<[stdin]>", "input filename (standard input if unset)" ) );
				V.push_back ( std::pair<std::string,std::string> ( "inputthreads=<[1]>", "input helper threads (for inputformat=bam only, default: 1)" ) );
				V.push_back ( std::pair<std::string,std::string> ( "reference=<>", "reference FastA (.fai file required, for cram i/o only)" ) );
				V.push_back ( std::pair<std::string,std::string> ( "range=<>", "coordinate range to be processed (for coordinate sorted indexed BAM input only)" ) );
				V.push_back ( std::pair<std::string,std::string> ( "outputthreads=<[1]>", "output helper threads (for outputformat=bam only, default: 1)" ) );
				V.push_back ( std::pair<std::string,std::string> ( "O=<[stdout]>", "output filename (standard output if unset)" ) );
				V.push_back ( std::pair<std::string,std::string> ( std::string("fixmates=<[")+::biobambam2::Licensing::formatNumber(getDefaultFixMates())+"]>", "fix mate information (for name collated input only, disabled by default)" ) );
				V.push_back ( std::pair<std::string,std::string> ( std::string("calmdnm=<[")+::biobambam2::Licensing::formatNumber(getDefaultCalMdNm())+"]>", "calculate MD and NM aux fields (for coordinate sorted output only)" ) );
				V.push_back ( std::pair<std::string,std::string> ( std::string("calmdnmreference=<[]>"), "reference for calculating MD and NM aux fields (calmdnm=1 only)" ) );
				V.push_back ( std::pair<std::string,std::string> ( std::string("calmdnmrecompindetonly=<[")+::biobambam2::Licensing::formatNumber(getDefaultCalMdNm())+"]>", "only recalculate MD and NM in the presence of indeterminate bases (calmdnm=1 only)" ) );
				V.push_back ( std::pair<std::string,std::string> ( std::string("calmdnmwarnchange=<[")+::biobambam2::Licensing::formatNumber(getDefaultCalMdNmWarnChange())+"]>", "warn when changing existing MD/NM fields (calmdnm=1 only)" ) );
				V.push_back ( std::pair<std::string,std::string> ( std::string("adddupmarksupport=<[")+::biobambam2::Licensing::formatNumber(getDefaultAddDupMarkSupport())+"]>", "add info for streaming duplicate marking (for name collated input only, ignored for fixmate=0, disabled by default)" ) );
				V.push_back ( std::pair<std::string,std::string> ( "tag=<[a-zA-Z][a-zA-Z0-9]>", "aux field id for tag string extraction (adddupmarksupport=1 only)" ) );
				V.push_back ( std::pair<std::string,std::string> ( "nucltag=<[a-zA-Z][a-zA-Z0-9]>", "aux field id for nucleotide tag extraction (adddupmarksupport=1 only)" ) );
				V.push_back ( std::pair<std::string,std::string> ( std::string("markduplicates=<[")+::biobambam2::Licensing::formatNumber(getDefaultMarkDuplicates())+"]>", "mark duplicates (only when input name collated and output coordinate sorted, disabled by default)" ) );
				V.push_back ( std::pair<std::string,std::string> ( std::string("rmdup=<[")+::biobambam2::Licensing::formatNumber(getDefaultRmDup())+"]>", "remove duplicates (only when input name collated and output coordinate sorted, disabled by default)" ) );
				V.push_back ( std::pair<std::string,std::string> ( std::string("streaming=<[")+::biobambam2::Licensing::formatNumber(getDefaultStreaming())+"]>", "do not open input files multiple times when set" ) );

				::biobambam2::Licensing::printMap(std::cerr,V);

				std::cerr << std::endl;
				return EXIT_SUCCESS;
			}

		return bamsort(arginfo);
	}
	catch(std::exception const & ex)
	{
		std::cerr << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}
